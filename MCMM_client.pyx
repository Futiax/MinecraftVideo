# distutils: language=c++
# cython: boundscheck=False
# cython: wraparound=False
# cython: nonecheck=False
# cython: cdivision=True
# cython: language_level=3

import os
from time import time
from json import load, dump
from pathlib import Path

import numpy as np
cimport numpy as np

from cython.parallel import prange
from libc.math cimport floor
from libc.stdlib cimport malloc, free
from libc.string cimport memcpy

import cv2
from requests import get
from PIL import Image
from nbt import nbt


ctypedef np.uint8_t DTYPE_t
DTYPE = np.uint8
ctypedef np.intp_t INTP_t
ctypedef long long LONG_t


cdef unsigned char[256][256][256] color_lookup_table

cdef void build_lookup_table(dict cmc) noexcept:
    """Construit une table de lookup 3D RGB->ID pour éviter les appels dict.get() coûteux"""
    cdef int r, g, b
    cdef unsigned char default_id = 41
    
    for r in range(256):
        for g in range(256):
            for b in range(256):
                color_lookup_table[r][g][b] = default_id
    
    for (r, g, b), id_str in cmc.items():
        color_lookup_table[r][g][b] = <unsigned char>int(id_str)


cdef void imgtodat(unsigned char[:, :, :] frame_view, LONG_t outputnum, INTP_t height, 
                   INTP_t width, str output_dir) noexcept:
    """Convertit une frame en fichiers .dat de cartes Minecraft en parallèle"""
    cdef:
        LONG_t nbrmap = height * width
        int i
        INTP_t ligne, colonne, pixeldl, pixeldc, y, x
        unsigned char r, g, b
        LONG_t pos
        unsigned char* id_array

    for i in prange(nbrmap, schedule='dynamic', num_threads=4, nogil=True):
        ligne = i // width
        colonne = i % width
        pixeldl = ligne * 128
        pixeldc = colonne * 128
        pos = outputnum + i - height * width
        
        id_array = <unsigned char*> malloc(16384 * sizeof(unsigned char))
        if not id_array:
            continue

        for y in range(128):
            for x in range(128):
                r = frame_view[pixeldl + y, pixeldc + x, 0]
                g = frame_view[pixeldl + y, pixeldc + x, 1]
                b = frame_view[pixeldl + y, pixeldc + x, 2]
                
                id_array[y * 128 + x] = color_lookup_table[r][g][b]

        with gil:
            write_nbt_file(f"{output_dir}/map_{pos}.dat", bytes(id_array[:16384]))
        
        free(id_array)


cdef void write_nbt_file(str filename, bytes color_data) noexcept:
    """Écrit un fichier NBT de carte Minecraft au format 1.21"""
    cdef object nbtfile
    nbtfile = nbt.NBTFile()
    nbtfile["data"] = nbt.TAG_Compound()
    nbtfile["DataVersion"] = nbt.TAG_Int(4440)
    nbtfile["data"]["xCenter"] = nbt.TAG_Int(0)
    nbtfile["data"]["zCenter"] = nbt.TAG_Int(0)
    nbtfile["data"]["trackingPosition"] = nbt.TAG_Byte(0)
    nbtfile["data"]["unlimitedTracking"] = nbt.TAG_Byte(0)
    nbtfile["data"]["dimension"] = nbt.TAG_String("futiax:videotomap")
    nbtfile["data"]["locked"] = nbt.TAG_Byte(1)
    nbtfile["data"]["colors"] = nbt.TAG_Byte_Array()
    nbtfile["data"]["colors"].value = color_data
    nbtfile.write_file(filename)


cdef np.ndarray[DTYPE_t, ndim=3] process_frame(np.ndarray[DTYPE_t, ndim=3] frame, 
                                                 tuple bg_color, INTP_t target_width, 
                                                 INTP_t target_height, list palette):
    """Redimensionne une frame et applique la palette Minecraft"""
    cdef:
        INTP_t height = frame.shape[0]
        INTP_t width = frame.shape[1]
        double scale = min(
            (target_width * 128.0) / width,
            (target_height * 128.0) / height
        )
        INTP_t new_width = <INTP_t>(width * scale)
        INTP_t new_height = <INTP_t>(height * scale)
        INTP_t target_width_px = target_width * 128
        INTP_t target_height_px = target_height * 128
        INTP_t x_offset = (target_width_px - new_width) // 2
        INTP_t y_offset = (target_height_px - new_height) // 2

    resized = cv2.resize(frame, (new_width, new_height), interpolation=cv2.INTER_AREA)
    
    result = np.full((target_height_px, target_width_px, 3), bg_color, dtype=DTYPE)
    result[y_offset:y_offset + new_height, x_offset:x_offset + new_width] = resized
    
    rgb_frame = cv2.cvtColor(result, cv2.COLOR_BGR2RGB)

    palette_image = Image.new('P', (1, 1))
    palette_image.putpalette(palette)
    result_pil = Image.fromarray(rgb_frame).quantize(palette=palette_image, dither=Image.Dither.NONE)
    
    final_result = np.array(result_pil.convert('RGB'), dtype=DTYPE)
    return final_result


cpdef void process_video(str video_path, INTP_t width, INTP_t height, INTP_t framerate):
    """Traite une vidéo et génère les fichiers de cartes Minecraft + audio"""
    cdef:
        LONG_t start_map_id = 0
        LONG_t num = start_map_id
        double frame_skip
        int frame_count = 0
        double video_frame_nbr, start_time, video_fps
        np.ndarray[DTYPE_t, ndim=3] frame, frame_processed
        unsigned char[:, :, :] frame_view
        dict cmc = {}
        list palette = []
        int idx, r, g, b
        unsigned char shade_r, shade_g, shade_b
        list shades = [180, 220, 255, 135]

    mc_dir = Path("../minecraft/saves/world").resolve()
    rp_dir = Path("../minecraft/resourcepacks/video_rp").resolve()
    palette_path = mc_dir / "datapacks/palette/data/gameboy/mapcolors/colors/preset_color_list.json"
    output_dir = mc_dir / "data"
    dp_audio_path = mc_dir / "datapacks/video_dp/data/video/jukebox_song/videoone.json"
    rp_audio_path = rp_dir / "assets/video/sounds/records/videoone.ogg"
    
    cdef str output_dir_str = str(output_dir)

    output_dir.mkdir(parents=True, exist_ok=True)
    rp_audio_path.parent.mkdir(parents=True, exist_ok=True)

    with open(palette_path) as f:
        color_list = load(f)["colorList"]
    
    for idx_str, hex_color in color_list.items():
        idx = int(idx_str)
        hex_color = hex_color.lstrip('#')
        r, g, b = int(hex_color[0:2], 16), int(hex_color[2:4], 16), int(hex_color[4:6], 16)
        for shade_idx, shade_val in enumerate(shades):
            shade_r = <unsigned char>min(255, (r * shade_val) // 255)
            shade_g = <unsigned char>min(255, (g * shade_val) // 255)
            shade_b = <unsigned char>min(255, (b * shade_val) // 255)
            palette.extend([shade_r, shade_g, shade_b])
            cmc[(shade_r, shade_g, shade_b)] = str(idx * 4 + shade_idx)

    build_lookup_table(cmc)

    capture = cv2.VideoCapture(video_path)
    video_fps = capture.get(cv2.CAP_PROP_FPS)
    video_frame_nbr = capture.get(cv2.CAP_PROP_FRAME_COUNT)
    
    with open(dp_audio_path, 'r+') as f:
        audio_json = load(f)
        audio_json["length_in_seconds"] = video_frame_nbr / video_fps
        f.seek(0); dump(audio_json, f, indent=4); f.truncate()
    
    # Extraction et conversion audio en Vorbis pour Minecraft
    print("Extraction de l'audio...")
    cmd = f'ffmpeg -y -i "{video_path}" -vn -c:a libvorbis -q:a 4 "{rp_audio_path}"'
    result = os.system(cmd)
    if result == 0:
        print("✓ Audio extrait et converti en OGG (Vorbis)")
    else:
        print(f"⚠ Avertissement: ffmpeg a retourné le code {result}")
        print("Le traitement vidéo continue...")
    
    frame_skip = video_fps / framerate if framerate > 0 else 1
    start_time = time()
    
    cdef double current_pos
    
    while capture.isOpened():
        ret, frame = capture.read()
        if not ret: 
            break
        
        current_pos = capture.get(cv2.CAP_PROP_POS_FRAMES)
        if round(current_pos % frame_skip) != 0: 
            continue

        frame_count += 1
        
        if frame_count % 10 == 0:
            elapsed = time() - start_time
            remaining = (elapsed / frame_count) * (video_frame_nbr / frame_skip - frame_count)
            print(f'Frame: {frame_count}/{int(video_frame_nbr / frame_skip)} | Restant: {remaining:.1f}s | {elapsed/frame_count*1000:.1f}ms/frame    ', end='\r')

        frame_processed = process_frame(frame, (0, 0, 0), width, height, palette)
        frame_view = frame_processed
        
        imgtodat(frame_view, num, height, width, output_dir_str)
        
        num += width * height

    capture.release()
    print("\n✓ Conversion terminée!")


cpdef void main():
    """Point d'entrée principal - gère le téléchargement et le traitement de la vidéo"""
    np.import_array()
    cdef str video_path = "video.mp4", url
    cdef INTP_t framerate, width, height
    
    while True:
        try:
            url = input("Entrez l'URL de la vidéo : ")
            framerate = int(input("Entrez le framerate désiré [1-20] : "))
            width = int(input("Entrez la largeur en nombre de cartes : "))
            height = int(input("Entrez la hauteur en nombre de cartes : "))

            if not (1 <= framerate <= 20 and width > 0 and height > 0):
                print("Valeurs invalides!"); continue
            
            print("Téléchargement...")
            r = get(url, stream=True); r.raise_for_status()
            with open(video_path, 'wb') as f:
                for chunk in r.iter_content(chunk_size=8192): 
                    f.write(chunk)
            
            process_video(video_path, width, height, framerate)
            
        except KeyboardInterrupt: 
            print("\nArrêté."); break
        except Exception as e: 
            print(f"\nErreur: {e}")


if __name__ == "__main__":
    main()