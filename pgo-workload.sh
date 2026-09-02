#!/bin/sh
TITTY="${1:-./titty}"

"$TITTY" -e sh -c '
seq 1 40000
for i in $(seq 0 255); do printf "\033[38;5;%sm#\033[48;5;%sm#\033[0m" "$i" "$i"; done
printf "\n"
i=1
while [ $i -le 800 ]; do
  printf "\033[38;2;%s;%s;%smX\033[0m" $((i%255)) $((i*3%255)) $((i*7%255))
  i=$((i+1))
done
printf "\n日本語テスト åäö ▀▄█▓▒░ ┌─┬─┐ ╔═╦═╗ ⣿⣶⣤⣀ 🎉\n"
tput smcup 2>/dev/null
i=1
while [ $i -le 400 ]; do
  tput cup $((i%20)) $((i%40)) 2>/dev/null
  printf "\033[1;3%smalt %s\033[0m" $((i%8)) "$i"
  i=$((i+1))
done
tput rmcup 2>/dev/null
printf "\033[5;20r"
i=1
while [ $i -le 600 ]; do printf "region %s\n" "$i"; i=$((i+1)); done
printf "\033[r"
seq 1 10000
'
