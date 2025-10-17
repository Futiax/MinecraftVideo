import MCMM_client as mcmm
import requests
import glob
import os
what_to_do = input("Entrer 1 pour traiter une vidéo local\n2Pour traiter une vidéo téléchargeable\n3Pour effacer les map générées\n") 
if what_to_do == "1":
    path = input("Entrer le chemin de la vidéo: ")
    mcmm.process_video("C:/Users/yux/downloads/PALADIUM_GO.mp4", 4, 3, 20)
if what_to_do == "2":
    video_url = input("Entrer l'url de la vidéo: ")
    video = requests.get(video_url)
    tmp = requests.head(video_url)
    if tmp.status_code != 200:
        print("L'url n'est pas valide")
    else:
       mcmm.process_video_from_url(video_url, 4, 3, 20)
if what_to_do == "3":
    fichiers = glob.glob("../minecraft/saves/world/data/video/maps/map_*.dat")
    len_fichiers = len(fichiers)
    i = 0
    for map_file in fichiers:
        i += 1
        print(f"Suppression ({i}/{len_fichiers}) ")
        os.remove(map_file)
    print("Suppression terminée")