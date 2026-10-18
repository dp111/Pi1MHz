#!/bin/sh -e
# Host test of storage=smb (fsroute.c, smb/smb_client.c, smb/smb_crypto.c,
# libsmb2) against a private Samba this script starts: signing mandatory,
# a user with a password, as a Windows 11 share is.  Skipped when smbd is
# not installed or the script does not run as root (smbd needs it).  Only
# the socket shim is the test's (host_sock_shim.c: real sockets for lwIP).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../cflags.sh"
SRC=${SRC_DIR:-$HERE/../..}
if ! command -v smbd >/dev/null 2>&1 || [ "$(id -u)" != 0 ]; then
   echo "SKIP: smb tests need smbd and root"
   exit 0
fi
B=$(mktemp -d)
S=$(mktemp -d /tmp/smbXXXX)            # short: smbd's socket paths have a length limit
PORT=$((20000 + $$ % 20000))
cleanup() { [ -f "$S/pid/smbd.pid" ] && kill "$(cat "$S/pid/smbd.pid")" 2>/dev/null; rm -rf "$B" "$S"; }
trap cleanup EXIT

mkdir -p "$S/private" "$S/lock" "$S/state" "$S/cache" "$S/pid" "$S/log" "$S/share"
cat > "$S/smb.conf" <<CONF
[global]
   workgroup = WORKGROUP
   server role = standalone server
   smb ports = $PORT
   bind interfaces only = yes
   interfaces = 127.0.0.1
   private dir = $S/private
   lock directory = $S/lock
   state directory = $S/state
   cache directory = $S/cache
   pid directory = $S/pid
   log file = $S/log/smbd.log
   passdb backend = tdbsam:$S/private/passdb.tdb
   server min protocol = SMB2_02
   server signing = mandatory
   map to guest = never
   load printers = no
   disable spoolss = yes
[Beeb]
   path = $S/share
   read only = no
   force user = root
CONF
printf 'beebpass\nbeebpass\n' | smbpasswd -c "$S/smb.conf" -s -a root >/dev/null
smbd -D --configfile="$S/smb.conf"
i=0; while ! (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null; do
   i=$((i + 1)); [ $i -gt 50 ] && { echo "smbd did not start"; cat "$S/log/smbd.log"; exit 1; }
   sleep 0.1
done

L=$SRC/smb/libsmb2
# libsmb2's client, as the firmware builds it: its AES and MD4 are
# replaced (smb_crypto.c), the share enumeration and Kerberos are not used
LIBSMB2_SRCS=$(cd "$L/lib" && ls *.c | grep -vxE 'aes\.c|aes_reference\.c|aes_apple\.c|md4c\.c|krb5-wrapper\.c|smb2-share-enum\.c|compat\.c' )
# On the host libsmb2 builds against glibc's own socket headers - only the
# firmware's config.h and the smbp_* renames are kept.
mkdir -p "$B/obj" "$B/lib" "$B/hostcfg"
cp "$SRC/smb/port/include/config.h" "$B/hostcfg/"
for f in $LIBSMB2_SRCS; do
   gcc -std=gnu11 -g -w -ffunction-sections -fdata-sections -DHAVE_CONFIG_H -D_U_= \
       -I"$B/hostcfg" -I"$L/include" -I"$L/include/smb2" -I"$L/lib" \
       -include "$SRC/smb/port/smb_port.h" -c "$L/lib/$f" -o "$B/lib/${f%.c}.o"
done
MB=$SRC/wifi/mbedtls
gcc -std=gnu11 -g -w -I"$SRC/wifi" -I"$MB/include" '-DMBEDTLS_CONFIG_FILE="mbedtls_config_pi1mhz.h"' \
    -c "$MB/library/aes.c" -o "$B/lib/mb_aes.o"
gcc -std=gnu11 -g -w -I"$SRC/wifi" -I"$MB/include" '-DMBEDTLS_CONFIG_FILE="mbedtls_config_pi1mhz.h"' \
    -c "$MB/library/platform_util.c" -o "$B/lib/mb_util.o"

# the firmware's own files, in a mirror of the tree with the test's stubs
# where the headers around them would be (they include "../config.h" etc.)
M=$B/tree
mkdir -p "$M/smb" "$M/BeebSCSI"
cp -r "$HERE"/stubs/. "$M/"
cp "$SRC"/net_dns.h "$SRC"/fsroute.c "$SRC"/fsroute.h "$M/"
cp "$SRC"/smb/smb_client.c "$SRC"/smb/smb_client.h "$SRC"/smb/smb_crypto.c "$SRC"/smb/smb_socket.h "$M/smb/"
ln -s "$SRC/smb/port" "$M/smb/port"
ln -s "$L" "$M/smb/libsmb2"
ln -s "$SRC/BeebSCSI/fatfs" "$M/BeebSCSI/fatfs"
MBF="-I$SRC/wifi -I$MB/include -DMBEDTLS_CONFIG_FILE=\"mbedtls_config_pi1mhz.h\""
gcc -std=gnu11 -g -Wall -Wextra -I"$M" $MBF -I"$L/include" -c "$M/smb/smb_crypto.c" -o "$B/obj/smb_crypto.o"
gcc -std=gnu11 -g -Wall -Wextra -I"$M" -I"$L/include" -c "$M/smb/smb_client.c" -o "$B/obj/smb_client.o"
gcc -std=gnu11 -g -Wall -Wextra -I"$M" -I"$L/include" -c "$M/fsroute.c" -o "$B/obj/fsroute.o"
gcc -std=gnu11 -g -Wall -I"$M/smb" -I"$HERE" -c "$HERE/host_sock_shim.c" -o "$B/lib/host_sock_shim.o"
gcc -std=gnu11 -g -Wall -c "$HERE/host_sock_real.c" -o "$B/lib/host_sock_real.o"
gcc -std=gnu11 -g -Wall -Wextra -I"$M" -I"$L/include" -c "$HERE/test_smb.c" -o "$B/obj/test_smb.o"
gcc -g -Wl,--gc-sections -o "$B/t" "$B"/obj/*.o "$B"/lib/*.o

# The FAT service on the share (test_fatsmb.c): its own tree - the services
# suite's stubs for the bus, real FatFs on a RAM disk for the card (with
# f_mkfs), the config parser fed the share's logon.
F=$B/fattree
mkdir -p "$F/smb" "$F/BeebSCSI/fatfs" "$B/fobj"
cp -r "$HERE"/stubs/. "$F/"
cp -r "$HERE"/../services/stubs/. "$F/"
rm -f "$F/BeebSCSI/fatfs/ff.h" "$F/config.h"
cat >> "$F/Pi1MHz.h" <<'STUB'
/* the one field smb_client.c reads: nRST edges counted by the IRQ */
typedef struct { uint32_t edges; } Pi1MHz_break_t;
extern volatile Pi1MHz_break_t Pi1MHz_break;
STUB
cp "$SRC"/net_dns.h "$SRC"/fsroute.c "$SRC"/fsroute.h "$SRC"/services_emulator.c "$SRC"/fat_service.c \
   "$SRC"/services.h "$SRC"/M5000_emulator.h "$SRC"/fujibus_service.h "$SRC"/config.c "$SRC"/config.h "$F/"
cp "$SRC"/smb/smb_client.c "$SRC"/smb/smb_client.h "$SRC"/smb/smb_crypto.c "$SRC"/smb/smb_socket.h "$F/smb/"
ln -s "$SRC/smb/port" "$F/smb/port"
ln -s "$L" "$F/smb/libsmb2"
cp "$SRC"/BeebSCSI/filesystem_storage.c "$F/BeebSCSI/"
cp "$SRC"/BeebSCSI/fatfs/ff.c "$SRC"/BeebSCSI/fatfs/ff.h "$SRC"/BeebSCSI/fatfs/ffunicode.c \
   "$SRC"/BeebSCSI/fatfs/diskio.h "$F/BeebSCSI/fatfs/"
sed 's/^#define FF_USE_MKFS[[:space:]]*0/#define FF_USE_MKFS 1/' \
   "$SRC"/BeebSCSI/fatfs/ffconf.h > "$F/BeebSCSI/fatfs/ffconf.h"
cp "$HERE"/test_fatsmb.c "$F/"
FW="-std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -I$F -I$L/include"
gcc $FW $MBF -c "$F/smb/smb_crypto.c" -o "$B/fobj/smb_crypto.o"
for f in smb/smb_client fsroute services_emulator fat_service config BeebSCSI/filesystem_storage \
         BeebSCSI/fatfs/ff BeebSCSI/fatfs/ffunicode test_fatsmb; do
   gcc $FW -c "$F/$f.c" -o "$B/fobj/$(basename $f).o"
done
gcc -g -Wl,--gc-sections -o "$B/tf" "$B"/fobj/*.o "$B"/lib/*.o

echo "== storage=smb: fsroute + libsmb2 against Samba (signing mandatory) =="
SMB_TEST_STOP="kill \$(cat $S/pid/smbd.pid); sleep 0.5" \
SMB_TEST_START="smbd -D --configfile=$S/smb.conf; sleep 1" \
SMB_TEST_FREEZE="pkill -STOP -P \$(cat $S/pid/smbd.pid); kill -STOP \$(cat $S/pid/smbd.pid)" \
SMB_TEST_THAW="kill -CONT \$(cat $S/pid/smbd.pid); pkill -CONT -P \$(cat $S/pid/smbd.pid)" \
SMB_TEST_SERVER=127.0.0.1:$PORT SMB_TEST_USER=root SMB_TEST_PASS=beebpass SMB_TEST_DIR=$S/share "$B/t"

echo "== the FAT service (MMFS) on the share =="
SMB_TEST_SERVER=127.0.0.1:$PORT SMB_TEST_USER=root SMB_TEST_PASS=beebpass SMB_TEST_DIR=$S/share "$B/tf"
