#!/bin/sh
# Verifie le format de flux v2 : un s64 big-endian de 8 octets (le PTS de la
# frame, en microsecondes) devant les tuiles de chaque frame.
#
#   ./test_pts.sh [chemin/vers/mcmm]
#
# La video de test est fabriquee avec -output_ts_offset 5 : ses PTS commencent
# a 5 s et non a 0. C'est tout l'interet du test. Un PTS synthetise a partir du
# compteur de frames vaudrait 0 ici (et 10 s apres --seek 10) : il passerait
# n'importe quel controle de monotonie et d'espacement, mais il echoue sur ces
# deux valeurs absolues. Sans cet offset le test ne distinguerait pas un vrai
# PTS lu dans la source d'un compteur deguise.
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

FPS=10
TILE=16384          # 128*128 index de palette
STRIDE=$((8 + TILE))  # 1x1 carte : 8 octets de PTS + une tuile
HEADER=16
STEP=$((1000000 / FPS))
TOL=5000            # microsecondes

fail() { echo "ECHEC: $*" >&2; exit 1; }

# |a - b| <= tol
near() {
    d=$(($1 - $2))
    [ $d -lt 0 ] && d=$((-d))
    [ $d -le $3 ]
}

be16() { od -A n -t u2 --endian=big -j "$1" -N 2 "$2" | tr -d ' '; }
be64() { od -A n -t d8 --endian=big -j "$1" -N 8 "$2" | tr -d ' '; }

# --- Fixtures -------------------------------------------------------------
# Piege connu du parseur : jamais de diese hors d'un code couleur, il le
# prendrait pour la couleur de l'id 0.
cat > "$WORK/palette.json" <<'EOF'
{
  "0": "#000000",
  "1": "#ff0000",
  "2": "#00ff00",
  "3": "#0000ff",
  "4": "#ffffff"
}
EOF

ffmpeg -hide_banner -v error -y -f lavfi -i testsrc=size=320x240:rate=30 \
    -t 20 -c:v libx264 -g 60 -pix_fmt yuv420p -output_ts_offset 5 \
    "$WORK/src.mp4"

# --- Conversion sans seek -------------------------------------------------
"$MCMM" --stream --palette "$WORK/palette.json" \
    "$WORK/src.mp4" 1 1 $FPS > "$WORK/a.bin" 2> "$WORK/a.log"

magic=$(dd if="$WORK/a.bin" bs=1 count=4 2>/dev/null)
[ "$magic" = "MCMM" ] || fail "magie attendue MCMM, lue '$magic'"

version=$(be16 4 "$WORK/a.bin")
[ "$version" = "2" ] || fail "version attendue 2, lue $version"

fps_hdr=$(be16 10 "$WORK/a.bin")
[ "$fps_hdr" = "$FPS" ] || fail "fps attendu $FPS, lu $fps_hdr"

size=$(wc -c < "$WORK/a.bin")
body=$((size - HEADER))
[ $((body % STRIDE)) -eq 0 ] \
    || fail "corps de $body octets, pas un multiple de $STRIDE (frame tronquee)"
frames=$((body / STRIDE))
[ $frames -ge 20 ] || fail "seulement $frames frames, il en faut au moins 20"

# PTS absolu de la premiere frame : 5 s, l'offset de la source.
first=$(be64 $HEADER "$WORK/a.bin")
near "$first" 5000000 200000 \
    || fail "premier PTS = $first us, attendu ~5000000 (PTS synthetise ? il vaudrait 0)"

# Monotonie stricte et espacement regulier sur les 20 premieres frames.
prev=$first
n=1
while [ $n -lt 20 ]; do
    pts=$(be64 $((HEADER + n * STRIDE)) "$WORK/a.bin")
    [ "$pts" -gt "$prev" ] \
        || fail "PTS non croissant a la frame $n : $prev puis $pts"
    near "$((pts - prev))" "$STEP" "$TOL" \
        || fail "ecart de $((pts - prev)) us a la frame $n, attendu ~$STEP"
    prev=$pts
    n=$((n + 1))
done

# --- Conversion avec --seek 10 --------------------------------------------
"$MCMM" --stream --palette "$WORK/palette.json" --seek 10 \
    "$WORK/src.mp4" 1 1 $FPS > "$WORK/b.bin" 2> "$WORK/b.log"

# 5 s d'offset de source + 10 s de seek. Le repli synthetise donnerait
# 10000000, et un PTS rebase par ffmpeg (sans -copyts) donnerait 0.
seeked=$(be64 $HEADER "$WORK/b.bin")
near "$seeked" 15000000 300000 \
    || fail "premier PTS apres --seek 10 = $seeked us, attendu ~15000000"

echo "OK — $frames frames, premier PTS ${first} us, apres seek ${seeked} us"
