#!/bin/bash
# ============================================================================
# 脚本名称: build_minimal_initramfs.sh
# 功能描述: 构建包含动态链接 init 的最小 initramfs (LoongArch32 ilp32s)
#
# 关键事实（由 ld.so strings 确认，glibc 2.27）：
#   ld.so 内嵌的默认搜索路径字符串为：/usr/lib/../lib32/sf/
#   规范化后 = /usr/lib32/sf/
#   路径解析逐组件进行：/usr/lib/../lib32/sf/libc.so.6 要求
#     - /usr/lib/ 目录必须存在（否则 open() 在第二段就 ENOENT）
#     - /usr/lib32/sf/libc.so.6 必须是实体文件
#
# init 源码：
#   默认使用与本脚本同目录的 init.c，可用环境变量 INIT_SRC 指定其他文件：
#     INIT_SRC=/path/to/init.c ./build_minimal_initramfs_dyn.sh [输出目录]
#
# 本脚本策略：
#   1. 解析 .interp 得到 ld.so 路径；解析 NEEDED 得到依赖名。
#   2. 主目标目录 = /usr/lib32/sf（ld.so 唯一认的路径）。
#   3. 兜底目标目录 = /lib32/sf、/lib32 等常见路径（实体副本，不用软链接）。
#   4. 打包前后均做 open() 路径解析验证，确认 /usr/lib/ 存在、libc 为实体 ELF。
# ============================================================================

set -e

# ---------- 颜色 ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

# ---------- 参数 ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INIT_SRC="${INIT_SRC:-${SCRIPT_DIR}/init.c}"
OUTPUT_DIR="${1:-$(pwd)}"
OUTPUT_CPIO="${OUTPUT_DIR}/rootfs_minimal.cpio"
OUTPUT_GZ="${OUTPUT_DIR}/rootfs_minimal.cpio.gz"

# ---------- 工具链 ----------
CROSS_COMPILE="${CROSS_COMPILE:-loongarch32r-linux-gnusf-}"
CC="${CROSS_COMPILE}gcc"
READELF="${CROSS_COMPILE}readelf"
STRINGS="${CROSS_COMPILE}strings"
command -v "${STRINGS}" &>/dev/null || STRINGS="strings"

SYSROOT="${SYSROOT:-/home/u/loongson-gnu-toolchain-8.3-x86_64-loongarch32r-linux-gnusf-v2.0/loongarch32r-linux-gnusf/sysroot}"

if ! command -v "${CC}" &> /dev/null; then
    echo -e "${RED}错误: 找不到 ${CC}${NC}"; exit 1
fi
if [ ! -d "${SYSROOT}" ]; then
    echo -e "${RED}错误: sysroot 不存在: ${SYSROOT}${NC}"; exit 1
fi
echo -e "${GREEN}使用 sysroot: ${SYSROOT}${NC}"

if [ ! -f "${INIT_SRC}" ]; then
    echo -e "${RED}错误: 找不到 init 源文件: ${INIT_SRC}${NC}"; exit 1
fi
INIT_SRC="$(readlink -f "${INIT_SRC}")"

# ---------- 临时目录 ----------
WORK_DIR=$(mktemp -d -t initramfs_build.XXXXXX)
echo -e "${GREEN}[1/7] 临时目录: ${WORK_DIR}${NC}"
trap "rm -rf ${WORK_DIR}" EXIT

# ---------- init.c ----------
echo -e "${GREEN}[2/7] init 源文件: ${INIT_SRC}${NC}"

# ---------- 编译 init ----------
echo -e "${GREEN}[3/7] 动态编译 init...${NC}"
${CC} -O2 -Wall -mabi=ilp32s -march=loongarch32r \
      --sysroot="${SYSROOT}" \
      -o "${WORK_DIR}/init" "${INIT_SRC}"
echo -e "      init 大小: $(du -h "${WORK_DIR}/init" | cut -f1)"

# ---------- 解析动态依赖 ----------
echo -e "${GREEN}[4/7] 解析动态链接器与依赖...${NC}"
INTERP=$(${READELF} -p .interp "${WORK_DIR}/init" 2>/dev/null \
         | sed -n 's/.*\]  \(.*\)/\1/p' | tr -d '\r')
[ -z "${INTERP}" ] && { echo -e "${RED}无法解析 .interp${NC}"; exit 1; }
INTERP_BASENAME=$(basename "${INTERP}")
INTERP_DIR="${INTERP%/*}"
echo -e "      .interp = ${INTERP}"
echo "------ readelf -d init（NEEDED / RPATH / RUNPATH）------"
${READELF} -d "${WORK_DIR}/init" | grep -E 'NEEDED|RPATH|RUNPATH' | sed 's/^/      /' || true

resolve() {
    for p in "$@"; do [ -e "$p" ] && { readlink -f "$p"; return 0; }; done
    return 1
}

INTERP_REAL=$(resolve \
    "${SYSROOT}${INTERP}" \
    "${SYSROOT}/lib32/${INTERP_BASENAME}" \
    "${SYSROOT}/lib32/sf/${INTERP_BASENAME}" \
    "${SYSROOT}/lib/${INTERP_BASENAME}") \
    || { echo -e "${RED}找不到 ld.so: ${INTERP_BASENAME}${NC}"; exit 1; }
echo -e "      ld.so 真实文件: ${INTERP_REAL}"

LIBC_REAL=$(resolve \
    "${SYSROOT}/lib32/sf/libc.so.6" \
    "${SYSROOT}/lib32/libc.so.6" \
    "${SYSROOT}/lib/libc.so.6") \
    || { echo -e "${RED}找不到 libc.so.6${NC}"; exit 1; }
echo -e "      libc 真实文件: ${LIBC_REAL}"

# ---------- 从 ld.so 提取默认搜索路径 ----------
echo -e "${GREEN}[5/7] 从 ld.so 提取默认搜索路径...${NC}"
LD_DEFAULT_DIRS=$("${STRINGS}" -a -n 3 "${INTERP_REAL}" 2>/dev/null \
    | grep -E '^/.*(lib|sf)' | sort -u)
echo "------ ld.so 内嵌路径（原始）------"
echo "${LD_DEFAULT_DIRS}" | sed 's/^/      /'

# 规范化：把 /usr/lib/../lib32/sf/ 变成 /usr/lib32/sf
NORMALIZED_DIRS=""
for d in ${LD_DEFAULT_DIRS}; do
    n=$(echo "${d}" | sed 's|/\.\./|/|g; s|//|/|g; s|/$||')
    NORMALIZED_DIRS="${NORMALIZED_DIRS} ${n}"
done
NORMALIZED_DIRS=$(echo ${NORMALIZED_DIRS} | tr ' ' '\n' | sort -u)
echo "------ 规范化后 ------"
echo "${NORMALIZED_DIRS}" | sed 's/^/      /'

# ---------- 收集依赖 ----------
echo -e "${GREEN}[6/7] 收集 NEEDED 与依赖...${NC}"
declare -a ALL_NEEDED
while IFS= read -r n; do [ -n "${n}" ] && ALL_NEEDED+=("${n}"); done < <( \
    ${READELF} -d "${WORK_DIR}/init" 2>/dev/null \
    | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' )

while IFS= read -r n; do
    [ -z "${n}" ] && continue
    [ "${n}" = "${INTERP_BASENAME}" ] && continue
    dup=0; for x in "${ALL_NEEDED[@]}"; do [ "${x}" = "${n}" ] && dup=1 && break; done
    [ "${dup}" -eq 0 ] && ALL_NEEDED+=("${n}")
done < <( ${READELF} -d "${LIBC_REAL}" 2>/dev/null \
    | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' )

declare -A LIB_REAL_PATH
LIB_REAL_PATH["libc.so.6"]="${LIBC_REAL}"
for n in "${ALL_NEEDED[@]}"; do
    [ "${n}" = "libc.so.6" ] && continue
    p=$(resolve \
        "${SYSROOT}/lib32/sf/${n}" \
        "${SYSROOT}/lib32/${n}" \
        "${SYSROOT}/lib/${n}" \
        "${SYSROOT}/usr/lib32/sf/${n}" \
        "${SYSROOT}/usr/lib32/${n}" \
        "${SYSROOT}/usr/lib/${n}") || {
        echo -e "${RED}找不到依赖 ${n}${NC}"; exit 1; }
    LIB_REAL_PATH["${n}"]="${p}"
    echo -e "      依赖: ${n} -> ${p}"
done

# ---------- 构建 rootfs ----------
echo -e "${GREEN}[7/7] 构建 rootfs...${NC}"
ROOTFS="${WORK_DIR}/rootfs"
rm -rf "${ROOTFS}"

# 主目标目录：/usr/lib32/sf（ld.so 唯一认的）
PRIMARY_DIR="/usr/lib32/sf"

# 收集所有需要 mkdir 的目录，去重
declare -a TARGET_DIRS=(
    "/usr/lib"              # ★ 必须存在，路径解析 /usr/lib/../... 才能继续
    "${PRIMARY_DIR}"        # /usr/lib32/sf
    "/lib32/sf"             # 兜底
    "/lib32"                # 兜底（.interp 所在目录）
    "/lib"                  # 兜底（某些 glibc 版本）
    "${INTERP_DIR}"         # .interp 指定目录
)
for d in ${NORMALIZED_DIRS}; do TARGET_DIRS+=("${d}"); done

declare -A SEEN
declare -a UNIQ_DIRS
for d in "${TARGET_DIRS[@]}"; do
    [ -z "${d}" ] && continue
    [ -n "${SEEN[$d]+x}" ] && continue
    SEEN["$d"]=1; UNIQ_DIRS+=("$d")
done

echo "------ 需要创建的目录 ------"
for d in "${UNIQ_DIRS[@]}"; do
    echo "      ${d}"
    mkdir -p "${ROOTFS}${d}"
done
mkdir -p "${ROOTFS}/dev" "${ROOTFS}/etc" "${ROOTFS}/proc" "${ROOTFS}/sys" \
         "${ROOTFS}/tmp" "${ROOTFS}/root" "${ROOTFS}/bin"
chmod 1777 "${ROOTFS}/tmp"

# init
cp "${WORK_DIR}/init" "${ROOTFS}/init"
chmod 0755 "${ROOTFS}/init"

# ld.so 放到 .interp 指定的路径（内核按此加载）
cp "${INTERP_REAL}" "${ROOTFS}${INTERP}"
chmod 0755 "${ROOTFS}${INTERP}"

# 每个依赖：实体拷贝到所有目标目录
for n in "${!LIB_REAL_PATH[@]}"; do
    src="${LIB_REAL_PATH[$n]}"
    for d in "${UNIQ_DIRS[@]}"; do
        dst="${ROOTFS}${d}/${n}"
        [ -f "${dst}" ] && continue
        cp "${src}" "${dst}"
        chmod 0755 "${dst}"
    done
done

# ld.so 副本也放到所有库目录（防某些 ld 在自身目录找 libc）
for d in "${UNIQ_DIRS[@]}"; do
    dst="${ROOTFS}${d}/${INTERP_BASENAME}"
    [ -f "${dst}" ] && continue
    cp "${INTERP_REAL}" "${dst}"
    chmod 0755 "${dst}"
done

# /dev/console
if [ "$EUID" -ne 0 ]; then
    sudo mknod -m 622 "${ROOTFS}/dev/console" c 5 1
else
    mknod -m 622 "${ROOTFS}/dev/console" c 5 1
fi

# ld.so.conf
cat > "${ROOTFS}/etc/ld.so.conf" << 'EOF'
/usr/lib32/sf
/lib32/sf
/lib32
/lib
EOF

# ---------- 打包前验证：模拟 ld.so 的 open() 路径解析 ----------
echo -e "${GREEN}打包前验证：模拟 ld.so 的 open() 路径解析...${NC}"
FAIL=0
verify_path() {
    local dir="$1" lib="$2"
    # 检查前缀目录链
    if [ ! -d "${ROOTFS}/usr/lib" ]; then
        echo -e "${RED}  ✗ /usr/lib 不存在（路径解析会在这一步失败）${NC}"
        FAIL=1; return
    fi
    # 规范化并检查
    local real
    real=$(cd "${ROOTFS}${dir}" 2>/dev/null && pwd -P) || {
        echo -e "${RED}  ✗ 目录不存在或不可进入: ${dir}${NC}"; FAIL=1; return; }
    real="${real#${ROOTFS}}"
    if [ ! -s "${ROOTFS}${real}/${lib}" ]; then
        echo -e "${RED}  ✗ 缺失或为空: ${real}/${lib}${NC}"; FAIL=1; return
    fi
    if [ -L "${ROOTFS}${real}/${lib}" ]; then
        echo -e "${YELLOW}  ! 是软链接（cpio 可能损坏）: ${real}/${lib}${NC}"
    fi
    local sz; sz=$(stat -c%s "${ROOTFS}${real}/${lib}")
    echo -e "${GREEN}  ✓ ${real}/${lib} (${sz} bytes)${NC}"
}

# 关键路径：/usr/lib/../lib32/sf/libc.so.6
verify_path "/usr/lib/../lib32/sf" "libc.so.6"
verify_path "${PRIMARY_DIR}" "libc.so.6"
verify_path "/lib32/sf" "libc.so.6"

# 每个依赖也验证主路径
for n in "${!LIB_REAL_PATH[@]}"; do
    [ "${n}" = "libc.so.6" ] && continue
    verify_path "${PRIMARY_DIR}" "${n}"
done

if [ "${FAIL}" -ne 0 ]; then
    echo -e "${RED}打包前验证失败，终止${NC}"
    exit 1
fi

# ---------- 打印 rootfs 结构 ----------
echo -e "${GREEN}rootfs 文件清单（库相关）:${NC}"
find "${ROOTFS}" -type f \( -name '*.so*' -o -name 'ld*' \) -print \
    | sed "s|${ROOTFS}||" | sort | sed 's/^/      /'

# ---------- 打包 ----------
echo -e "${GREEN}打包 cpio...${NC}"
cd "${ROOTFS}"
find . -print | cpio -o -H newc > "${OUTPUT_CPIO}"
cd - > /dev/null
echo -e "      未压缩: ${OUTPUT_CPIO} ($(du -h "${OUTPUT_CPIO}" | cut -f1))"

gzip -c "${OUTPUT_CPIO}" > "${OUTPUT_GZ}"
echo -e "      压缩:   ${OUTPUT_GZ} ($(du -h "${OUTPUT_GZ}" | cut -f1))"

# ---------- 打包后二次验证：解压确认 ----------
echo -e "${GREEN}打包后验证：解压 cpio 检查关键路径...${NC}"
VERIFY=$(mktemp -d -t initramfs_verify.XXXXXX)
( cd "${VERIFY}" && cpio -idm -H newc < "${OUTPUT_CPIO}" >/dev/null 2>&1 )

FAIL=0
vcheck() {
    local p="$1"
    if [ ! -s "${VERIFY}${p}" ]; then
        echo -e "${RED}  ✗ 解包后缺失: ${p}${NC}"; FAIL=1; return
    fi
    if [ -L "${VERIFY}${p}" ]; then
        echo -e "${YELLOW}  ! 解包后是软链接: ${p}${NC}"
    fi
    echo -e "${GREEN}  ✓ ${p} ($(stat -c%s "${VERIFY}${p}") bytes)${NC}"
}
vcheck "/init"
vcheck "${INTERP}"
vcheck "/usr/lib32/sf/libc.so.6"
vcheck "/lib32/sf/libc.so.6"
# 验证 /usr/lib/../lib32/sf 路径可解析
if [ -d "${VERIFY}/usr/lib" ]; then
    REAL=$(cd "${VERIFY}/usr/lib/../lib32/sf" 2>/dev/null && pwd -P)
    if [ -n "${REAL}" ] && [ -s "${REAL}/libc.so.6" ]; then
        echo -e "${GREEN}  ✓ /usr/lib/../lib32/sf/libc.so.6 可解析${NC}"
    else
        echo -e "${RED}  ✗ /usr/lib/../lib32/sf/libc.so.6 无法解析${NC}"
        FAIL=1
    fi
else
    echo -e "${RED}  ✗ 解包后 /usr/lib 不存在${NC}"
    FAIL=1
fi
rm -rf "${VERIFY}"

if [ "${FAIL}" -ne 0 ]; then
    echo -e "${RED}解包后验证失败${NC}"
    exit 1
fi

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}构建成功${NC}"
echo -e "  未压缩: ${OUTPUT_CPIO}"
echo -e "  压缩:   ${OUTPUT_GZ}"
echo -e "${GREEN}========================================${NC}"
echo "下一步:"
echo "1. 确认内核 .config 中 CONFIG_INITRAMFS_SOURCE 指向 ${OUTPUT_CPIO}"
echo "2. 重新编译内核（initramfs 在编译时嵌入）"
echo "3. 若之前编过，建议 make clean 或至少重新 make 触发 initramfs 重建"
