# 🎬 MinecraftVideo

Play videos in Minecraft using custom maps! This project converts any video into a format playable in Minecraft through the map system, complete with audio synchronization.

## ✨ Features

-   🎥 **Video to Minecraft Maps**: Converts videos frame-by-frame into Minecraft map items
-   🎵 **Audio Synchronization**: Extracts and converts audio to Minecraft's jukebox format
-   🎨 **Custom Color Palette**: Uses a custom color palette for accurate color reproduction
-   ⚡ **High Performance**: Written in Cython with parallel processing for fast conversion
-   🖼️ **Flexible Resolution**: Support for custom map grid sizes (width × height)
-   🎞️ **Adjustable Framerate**: Choose your desired framerate (1-20 FPS)

## 🔧 Requirements

### Python Dependencies

```bash
pip install numpy opencv-python pillow requests nbt cython
```

### External Tools

-   **FFmpeg**: Required for audio extraction and conversion
-   **Minecraft Java Edition**: Version 1.21.8 or compatible

### Minecraft Setup

-   **[Improved Map Colors](https://modrinth.com/mod/improved-map-colors)** : mod is required for this version.
-   The project expects the following directory structure:

```
../minecraft/
├── saves/world/
│   ├── data/                          # Generated map files
│   └── datapacks/
│       ├── palette/                   # Color palette datapack
│       └── video_dp/                  # Video audio datapack
└── resourcepacks/video_rp/            # Video resource pack
```

## 🚀 Installation

1. **Clone the repository**

    ```bash
    git clone https://github.com/yourusername/MinecraftVideo.git
    cd MinecraftVideo
    ```

2. **Compile the Cython code**

    ```bash
    python setup.py build_ext --inplace
    ```

3. **Install required datapacks and resource packs** in your Minecraft world

## 💻 Usage

Run the exemple script:

```bash
python exemple.py
```

You'll be prompted to enter:

1. **Video URL**: Direct link to the video file
2. **Framerate**: Desired playback framerate (1-20 FPS)
3. **Width**: Number of maps horizontally
4. **Height**: Number of maps vertically

### Example

```
Entrez l'URL de la vidéo : https://example.com/video.mp4
Entrez le framerate désiré [1-20] : 20
Entrez la largeur en nombre de cartes : 4
Entrez la hauteur en nombre de cartes : 3
```

This will create a 4×3 grid of maps (512×384 pixels) playing at 20 FPS (recommended).

## 🎮 In-Game Setup

1. Place the generated map files in your world's `data` folder
2. Use item frames to arrange maps in the desired grid pattern
3. Play the jukebox song to sync audio with the video

## ⚙️ How It Works

1. **Video Download**: Downloads the video from the provided URL
2. **Audio Extraction**: Uses FFmpeg to extract and convert audio to OGG format
3. **Frame Processing**:
    - Reads video frames with OpenCV
    - Resizes and centers frames to fit the map grid
    - Applies Minecraft's color palette quantization
    - Converts RGB colors to Minecraft map color IDs
4. **Map Generation**: Creates NBT files for each map with the converted pixel data
5. **Parallel Processing**: Uses multi-threading for fast conversion

## 🎨 Color Palette

The project uses a custom color 64 base color palette defined in:

```
datapacks/palette/data/gameboy/mapcolors/colors/preset_color_list.json
```

Made possible by using [Improved Map Colors](https://modrinth.com/mod/improved-map-colors) mod.
Each color has 4 shade variants (180, 220, 255, 135) define by minecraft code.

## 📊 Performance

-   **Lookup Table**: Pre-computed 3D RGB→ID lookup table for O(1) color conversion
-   **Parallel Processing**: Multi-threaded map generation (4 threads by default)
-   **Optimized Memory**: Efficient buffer management with Cython
-   **Typical Speed**: ~10-100ms per frame depending on resolution

## 🐛 Troubleshooting

-   **FFmpeg not found**: Make sure FFmpeg is installed and in your system PATH
-   **File not found errors**: Check that your Minecraft directory structure matches the expected layout
-   **Performance issues**: Reduce framerate or map grid size
-   **Color issues**: Verify your custom palette datapack is properly installed

## 📝 License

This project is licensed under the [Creative Commons Attribution-NonCommercial 4.0 International License (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/) **with commercial use exceptions**.

**You are free to:**

-   ✅ Share and redistribute
-   ✅ Modify and adapt
-   ✅ Use for personal/educational projects

**Under these conditions:**

-   📝 Give appropriate credit
-   🚫 No commercial use (unless explicitly authorized)
-   ⚠️ The author is NOT responsible for any misuse of this software

### Commercial Use

Commercial use is **prohibited by default** but can be authorized on a case-by-case basis.
If you wish to use this project commercially, please one of us for permission.

**To request commercial use authorization:**

-   Open an issue on GitHub with tag `[Commercial Request]`
-   Or contact: [votre email]

### Disclaimer

This software is provided "AS IS" without warranty of any kind. The author:

-   ❌ Is NOT responsible for how this software is used
-   ❌ Does NOT endorse any particular use or content processed
-   ❌ Is NOT liable for any damages or legal issues arising from its use
-   ⚠️ Reserves the right to REVOKE authorization for specific uses (racism, homophobia, nsfw, harrasment, etc)

**Users are responsible for:**

-   Ensuring they have rights to process any video content
-   Complying with copyright laws and content regulations
-   Using the software ethically and legally

By using this software, you agree to hold the author harmless from any claims.

## 📧 Contact

For questions or suggestions, please open an issue on GitHub.

---

**Note**: This project is designed for Minecraft 1.21+ with custom datapack support. Older versions may require modifications.
