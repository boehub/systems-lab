# wordfreq.sh FILE...  --  the ten most common words in the files, using only msh features.
#
#   ./msh examples/wordfreq.sh README.md

usage() {
  cat <<USAGE
usage: $0 FILE...
prints the ten most common words, with counts
USAGE
}

if [ $# -eq 0 ]; then
  usage
  exit 2
fi

total=0
for f in "$@"; do
  if [ ! -f "$f" ]; then
    echo "$0: $f: not a file" >&2
    continue
  fi
  n=$(wc -w < "$f")
  total=$((total + n))
  echo "$f: $n words"
done

echo "total: $total words"
echo

for f in "$@"; do
  [ -f "$f" ] && cat "$f"
done | tr -cs 'A-Za-z' '\n' | tr 'A-Z' 'a-z' | sort | uniq -c | sort -rn | head -n 10 |
  while read count word; do
    case "$word" in
      "") ;;
      *) echo "$count $word" ;;
    esac
  done
