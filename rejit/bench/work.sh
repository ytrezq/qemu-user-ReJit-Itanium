#!/bin/bash
# charges de travail (identiques pour ppc64le / aarch64 / armhf / riscv64 ; rpm+dnf ou dpkg+apt selon la distribution)
cd /tmp
if [ ! -f big.bin ]; then
  if [ -d /usr/lib64 ] && ls /usr/lib64/libpython3*.so.1.0 >/dev/null 2>&1; then
    cat /usr/lib64/libcrypto.so.3* /usr/lib64/libpython3.*.so.1.0 /usr/bin/bash > big.bin
  else
    cat /usr/lib/*-linux-gnu*/libcrypto.so.3 $(readlink -f /usr/bin/python3) /usr/bin/bash > big.bin
  fi
fi
[ -f lines.txt ] || (find /usr/share -type f | head -40000; seq 1 60000) > lines.txt
case "$1" in
 python)  python3 -c "import json,re; d=[{'a':i,'b':str(i)*3} for i in range(150000)]; s=json.dumps(d); json.loads(s); print(len(re.findall(r'\d+', s)))";;
 pyfp)    python3 -c "import math; print(sum(math.sin(i)*math.sqrt(i)+math.log(i+1) for i in range(600000)))";;
 gzip)    gzip -6 -c big.bin > /dev/null;;
 xz)      head -c 8000000 big.bin | xz -2 -c > /dev/null;;
 sort)    sort lines.txt > /dev/null; sort -n lines.txt > /dev/null;;
 sha)     sha256sum big.bin; md5sum big.bin;;
 awk)     awk 'BEGIN{s=0; for(i=1;i<2000000;i++) s+=sqrt(i)/i+sin(i); print s}';;
 grep)    grep -c -E 'lib[a-z]+[0-9]' lines.txt; grep -c -F 'share' lines.txt; grep -a -c ELF big.bin;;
 rpm)     if command -v rpm >/dev/null; then rpm -qa > /dev/null; rpm -qa --qf '%{NAME} %{SIZE}\n' | sort -k2 -n | tail -1;
          else dpkg -l > /dev/null; dpkg-query -W -f '${Package} ${Installed-Size}\n' | sort -k2 -n | tail -1; fi;;
 dnf)     if command -v dnf >/dev/null; then dnf --version; else apt-cache policy python3 bash; apt-cache depends python3 > /dev/null; fi;;
esac
