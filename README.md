# 🎬 MinecraftVideo

Play videos in Minecraft using custom maps! This project converts any video into a format playable in Minecraft through the map system, complete with audio synchronization.

> **🚧 Roadmap**: The Python/Cython implementation is being replaced by a native C version (see `c version/`), with no Python dependencies (FFmpeg libraries + zlib + OpenMP only). The Python version will be deprecated once the C version is complete.

## ✨ Features

-   🎥 **Video to Minecraft Maps**: Converts videos frame-by-frame into Minecraft map items
-   🎵 **Audio Synchronization**: Extracts and converts audio to Minecraft's jukebox format
-   🎨 **Custom Color Palette**: Uses a custom color palette for accurate color reproduction
-   ⚡ **High Performance**: Written in Cython with parallel processing for fast conversion
-   🖼️ **Flexible Resolution**: Support for custom map grid sizes (width × height)
-   🎞️ **Adjustable Framerate**: Choose your desired framerate (1-20 FPS)

## 🔧 Requirements

### C version (recommended)

-   **CMake** ≥ 3.16 and a C compiler (GCC/Clang/MSVC)
-   **zlib** development headers (`sudo apt install zlib1g-dev` on Debian/Ubuntu)
-   **FFmpeg**: `ffmpeg` and `ffprobe` must be available in your `PATH`
    -   Linux: `sudo apt install ffmpeg` (or your distro's equivalent)
    -   Windows: download a build from [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) — FFmpeg binaries are **not** bundled in this repository (size and redistribution licensing)

### Python version (legacy, will be deprecated)

```bash
pip install numpy opencv-python pillow requests nbt cython
```

### Common

-   **Minecraft Java Edition**: Version 1.21.8 or compatible

### Minecraft Setup

-   **[Improved Map Colors](https://modrinth.com/mod/improved-map-colors)** : mod is required for this version.
-   **WARNING** Currently audio is bugged so you need to made it using [IMD](https://github.com/TeamTernate/infinite-music-discs)
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
    git clone https://github.com/Futiax/MinecraftVideo.git
    cd MinecraftVideo
    ```

2. **Build the C version (Linux/macOS)**

    ```bash
    sudo apt install cmake gcc zlib1g-dev ffmpeg   # Debian/Ubuntu
    ./run.sh                                        # builds then runs
    ```

    Or manually:

    ```bash
    cmake -S "c version" -B "c version/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "c version/build"
    ```

    **Windows**: use `run.bat`, or build with CMake + MSVC/MinGW (FFmpeg must be in your `PATH`).

    _Legacy Python version_: `python setup.py build_ext --inplace`

3. **Install required datapacks and resource packs** in your Minecraft world

## 💻 Usage

### C version

```bash
./run.sh                          # interactive mode
./run.sh video.mp4 4 3 20         # <video> <width> <height> <fps>
```

### Python version (legacy)

```bash
python exemple.py
```

You'll be prompted to enter:

1. **Video path or URL**: Local file or direct link to the video file
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

This project is licensed under the [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/). See the [LICENSE](LICENSE) file for the full text.

**You are free to:**

-   ✅ Share and redistribute
-   ✅ Modify and adapt
-   ✅ Use for personal/educational projects

**Under these conditions:**

-   📝 Give appropriate credit
-   🔄 Share adaptations under the same license (ShareAlike)
-   🚫 No commercial use (unless explicitly authorized in writing — see below)

### 🚫 Commercial Use & Advertising

**Any commercial use is prohibited without prior written authorization.** This explicitly includes:

-   Using this project (or content generated with it) in **advertising or marketing** of any kind — in-game ad displays/billboards, sponsored content, promotional videos, brand campaigns
-   Selling access to, or monetizing, servers/maps/content built with this project
-   Bundling it into any paid product or service

Authorization may be granted case by case. To request it, open an issue on GitHub with the tag `[Commercial Request]`.

### ⚠️ Non-Endorsement & Unacceptable Uses

Use of this project **does not imply any association with, or endorsement by, the author** (see Section 2(a)(6) of the license). Do not present your use as affiliated with or approved by this project or its author.

In particular, the author does not consent to this project being used for:

-   **Political campaigns, propaganda, or partisan messaging** of any kind
-   **Advertising** (see above)
-   Hateful, discriminatory, harassing, or NSFW content

Any authorization granted may be **revoked** if the project is used for such purposes.

### Disclaimer

This software is provided "AS IS" without warranty of any kind. The author:

-   ❌ Is NOT responsible for how this software is used
-   ❌ Does NOT endorse any content processed or displayed with it
-   ❌ Is NOT liable for any damages or legal issues arising from its use

**Users are responsible for:**

-   Ensuring they have rights to process any video content
-   Complying with copyright laws and content regulations
-   Using the software ethically and legally

By using this software, you agree to hold the author harmless from any claims.

## 📧 Contact

For questions or suggestions, please open an issue on GitHub.

---

**Note**: This project is designed for Minecraft 1.21+ with custom datapack support. Older versions may require modifications.
