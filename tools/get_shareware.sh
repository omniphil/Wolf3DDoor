#!/bin/sh
# get_shareware.sh -- fetches the Wolfenstein 3D shareware episode (v1.4, the eight *.WL1 files) into data/.
#
#     sh tools/get_shareware.sh
#
# The data isn't in the repository: it is id Software's, given away as shareware to be passed on unchanged, and never
# became free software (only the engine did). This takes it from the Internet Archive's copy of the shareware release
# (item "wolf3dsw", from the CWI floppy disks) and checks it is exactly that release before using it. It needs curl and
# python3. Then:  python3 tools/mkpak.py data door/wolf3d.pak
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
url=https://archive.org/download/wolf3dsw/wolf3dsw.zip
want=44729c473432d11b9194f52c648ea19d
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "Downloading $url"
curl -fsSL -o "$tmp/wolf3dsw.zip" "$url"
got=$(python3 -c "import hashlib,sys; print(hashlib.md5(open(sys.argv[1],'rb').read()).hexdigest())" "$tmp/wolf3dsw.zip")
if [ "$got" != "$want" ]; then
    echo "The download isn't the expected shareware release (md5 $got, wanted $want)." >&2
    exit 1
fi
mkdir -p "$here/data"
python3 - "$tmp/wolf3dsw.zip" "$here/data" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
names = [n for n in z.namelist() if n.upper().endswith('.WL1')]
if len(names) != 8:
    sys.exit(f'expected 8 .WL1 files, found {len(names)}')
for n in names:
    with open(f'{sys.argv[2]}/{n.split("/")[-1].upper()}', 'wb') as f:
        f.write(z.read(n))
print(f'{len(names)} files into {sys.argv[2]}')
PY
