#!/bin/sh
# Verifie le cache disque de la LUT.
#
#   ./test_lut_cache.sh [chemin/vers/mcmm]
#
# Les deux proprietes qui comptent :
#   1. relancer sur la meme palette recharge le cache et rend une sortie
#      identique au bit pres — le cache ne doit rien changer au resultat ;
#   2. editer la palette invalide le cache. C'est le vrai piege : un cache mal
#      invalide resservirait l'ancienne LUT et donnerait des couleurs fausses
#      en silence, sans rien casser d'observable. Le test reecrit la palette
#      AU MEME CHEMIN, donc le cache porte le meme nom de fichier : une
#      invalidation qui reposerait sur le chemin ou la date passerait a cote.
set -e

MCMM=$1
if [ -z "$MCMM" ]; then
    for candidate in ./build-static/mcmm ./build/mcmm ./mcmm; do
        if [ -x "$candidate" ]; then MCMM=$candidate; break; fi
    done
fi
if [ ! -x "$MCMM" ]; then
    echo "mcmm introuvable — construis-le ou passe son chemin en argument." >&2
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

PALETTE=$WORK/palette.json
CACHE=$PALETTE.lut
LUT_BYTES=16777216

fail() { echo "ECHEC: $*" >&2; exit 1; }

# $1 = fichier de sortie, $2 = fichier de log
run() {
    "$MCMM" --stream --palette "$PALETTE" "$WORK/src.mp4" 1 1 5 > "$1" 2> "$2"
}

# Piege connu du parseur : jamais de diese hors d'un code couleur, il le
# prendrait pour la couleur de l'id 0.
write_palette_a() {
    cat > "$PALETTE" <<'EOF'
{
  "0": "#000000",
  "1": "#ff0000",
  "2": "#00ff00",
  "3": "#0000ff",
  "4": "#ffffff"
}
EOF
}

# Meme chemin, contenu different : autre id ET autre couleur, pour que la
# difference se voie forcement dans les tuiles.
write_palette_b() {
    cat > "$PALETTE" <<'EOF'
{
  "0": "#000000",
  "1": "#ff0000",
  "2": "#00ff00",
  "3": "#0000ff",
  "9": "#ffff00"
}
EOF
}

ffmpeg -hide_banner -v error -y -f lavfi -i testsrc=size=320x240:rate=30 \
    -t 2 -c:v libx264 -pix_fmt yuv420p "$WORK/src.mp4"

# --- 1. Premier lancement : rien en cache, la LUT se construit --------------
write_palette_a
run "$WORK/a1.bin" "$WORK/a1.log"
grep -q "Construction de la LUT" "$WORK/a1.log" \
    || fail "sans cache, la LUT aurait du etre construite"
[ -f "$CACHE" ] || fail "le cache n'a pas ete ecrit"

size=$(wc -c < "$CACHE")
[ "$size" -gt "$LUT_BYTES" ] \
    || fail "cache de $size octets, il doit contenir la LUT ($LUT_BYTES) et son en-tete"

# --- 2. Relance : le cache sert, la sortie ne bouge pas ---------------------
run "$WORK/a2.bin" "$WORK/a2.log"
grep -q "cache" "$WORK/a2.log" \
    || fail "le cache existait mais la LUT a ete reconstruite"
cmp -s "$WORK/a1.bin" "$WORK/a2.bin" \
    || fail "la sortie du cache differe de la sortie construite"

# --- 3. Palette editee au meme chemin : le cache doit etre invalide ---------
write_palette_b
run "$WORK/b1.bin" "$WORK/b1.log"
grep -q "Construction de la LUT" "$WORK/b1.log" \
    || fail "palette modifiee : l'ancienne LUT a ete resservie"
if cmp -s "$WORK/a1.bin" "$WORK/b1.bin"; then
    fail "palette modifiee mais tuiles identiques : les couleurs sont fausses"
fi

# Et le nouveau cache prend bien le relais.
run "$WORK/b2.bin" "$WORK/b2.log"
grep -q "cache" "$WORK/b2.log" || fail "le cache reconstruit n'est pas relu"
cmp -s "$WORK/b1.bin" "$WORK/b2.bin" || fail "cache reconstruit incoherent"

# --- 4. Cache corrompu : on reconstruit, on ne plante pas -------------------
dd if=/dev/zero of="$CACHE" bs=1 count=100 conv=notrunc status=none
run "$WORK/c1.bin" "$WORK/c1.log"
grep -q "Construction de la LUT" "$WORK/c1.log" \
    || fail "cache corrompu : il aurait fallu reconstruire"
cmp -s "$WORK/b1.bin" "$WORK/c1.bin" \
    || fail "la reconstruction apres corruption ne rend pas la meme sortie"

ls "$WORK"/*.tmp.* >/dev/null 2>&1 && fail "un fichier temporaire de cache traine"

echo "OK — cache relu, invalide a l'edition de la palette, refait apres corruption"
