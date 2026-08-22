# Disclaimer — AI assistance

AI was used on parts of this repository. I'd rather say it plainly than have
someone guess.

## The history of this code

1. **2023** — [McMovieMaker](https://github.com/Futiax/McMovieMaker): a Python
   script I wrote by hand, baking videos into map `.dat` files and a datapack.
2. **`MCMM_client.pyx`** — the same program rewritten in Cython for speed
   (lookup table, `prange`, custom palette).
3. **`c version/mcmm.c`** — a native C rewrite, no Python dependencies.

The names travelled with the code: `process_video` comes straight from the 2023
script and is still in the C file, along with `cmc`, `imgtodat`, `ligne`,
`colonne` in the intermediate versions.

## What is mine

The design, and it predates every AI-assisted line: quantization onto the map
palette, the `base × {180, 220, 255, 135}` shade structure and the
`id = index * 4 + shade` layout, tile indexing, the streaming protocol used by the
[Paper plugin](https://github.com/Futiax/VMC), the choice of dependencies. The
Python and Cython versions in this repository are my own work.

## What the model did

Parts of the initial Python → C translation, and debugging help. I asked for it
where translating my own code by hand would have been tedious — the algorithms
being translated were already written and working.

If that disqualifies the project in your eyes, that's a legitimate position. The
2023 history is public; judge from it.
