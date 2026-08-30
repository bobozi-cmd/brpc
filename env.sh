#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
WORKSPACE_DIR="${DEV_WORKSPACE_DIR:-${SCRIPT_DIR}}"
DEV_USER="${DEV_CONTAINER_USER:-developer}"
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"

[[ -d "${WORKSPACE_DIR}" ]] || {
  printf '错误：工作区不存在：%s\n' "${WORKSPACE_DIR}" >&2
  exit 1
}
WORKSPACE_DIR="$(cd -- "${WORKSPACE_DIR}" && pwd -P)"
BUILD_DIR="${DEV_BUILD_DIR:-${WORKSPACE_DIR}/build}"
BUILD_TYPE="${DEV_BUILD_TYPE:-RelWithDebInfo}"
if [[ "${BUILD_DIR}" != /* ]]; then
  BUILD_DIR="${WORKSPACE_DIR}/${BUILD_DIR}"
fi

PROJECT_NAME="${SCRIPT_DIR##*/}"
PROJECT_NAME="$(printf '%s' "${PROJECT_NAME}" | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9_.-' '-')"
PROJECT_KEY="$(printf '%s' "${WORKSPACE_DIR}" | cksum | awk '{print $1}')"

IMAGE_NAME="${DEV_IMAGE_NAME:-${PROJECT_NAME}-dev:ubuntu24.04}"
CONTAINER_NAME="${DEV_CONTAINER_NAME:-${PROJECT_NAME}-dev-${PROJECT_KEY}}"
CCACHE_HOST_DIR="${DEV_CCACHE_DIR:-${HOME}/.cache/${PROJECT_NAME}-ccache}"
DOCKERFILE="${SCRIPT_DIR}/docker/read.Dockerfile"

if [[ -n "${DEV_DOCKER_PLATFORM:-}" ]]; then
  DOCKER_PLATFORM="${DEV_DOCKER_PLATFORM}"
elif [[ "$(uname -m)" == "arm64" ]]; then
  DOCKER_PLATFORM="linux/arm64"
else
  DOCKER_PLATFORM="linux/amd64"
fi

log() {
  printf '[dev] %s\n' "$*"
}

die() {
  printf '[dev] 错误：%s\n' "$*" >&2
  exit 1
}

require_docker() {
  command -v docker >/dev/null 2>&1 || die "未找到 docker 命令"
  docker info >/dev/null 2>&1 || die "Docker daemon 未运行，或当前用户无权访问"
}

container_exists() {
  docker inspect "${CONTAINER_NAME}" >/dev/null 2>&1
}

container_running() {
  [[ "$(docker inspect --format '{{.State.Running}}' "${CONTAINER_NAME}" 2>/dev/null || true)" == "true" ]]
}

image_exists() {
  docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1
}

container_uses_current_image() {
  [[ "$(docker inspect --format '{{.Image}}' "${CONTAINER_NAME}" 2>/dev/null || true)" == \
     "$(docker image inspect --format '{{.Id}}' "${IMAGE_NAME}" 2>/dev/null || true)" ]]
}

git_common_dir() {
  local common_dir
  common_dir="$(git -C "${WORKSPACE_DIR}" rev-parse --git-common-dir 2>/dev/null || true)"
  [[ -n "${common_dir}" ]] || return 0

  if [[ "${common_dir}" != /* ]]; then
    common_dir="${WORKSPACE_DIR}/${common_dir}"
  fi
  (cd -- "${common_dir}" && pwd -P)
}

build_image() {
  require_docker
  [[ -f "${DOCKERFILE}" ]] || die "找不到 ${DOCKERFILE}"

  log "构建镜像 ${IMAGE_NAME} (${DOCKER_PLATFORM})"
  docker build \
    --platform "${DOCKER_PLATFORM}" \
    --build-arg "DEV_USER=${DEV_USER}" \
    --build-arg "DEV_UID=${HOST_UID}" \
    --build-arg "DEV_GID=${HOST_GID}" \
    --tag "${IMAGE_NAME}" \
    --file "${DOCKERFILE}" \
    "$@" \
    "${SCRIPT_DIR}/docker"
}

create_container() {
  local common_dir
  local -a run_args

  image_exists || build_image
  mkdir -p -- "${CCACHE_HOST_DIR}"

  run_args=(
    run --detach --init
    --name "${CONTAINER_NAME}"
    --hostname "${PROJECT_NAME}-dev"
    --platform "${DOCKER_PLATFORM}"
    --workdir "${WORKSPACE_DIR}"
    --label "dev.workspace=${WORKSPACE_DIR}"
    --env "DEVCONTAINER=1"
    --env "CCACHE_DIR=/home/${DEV_USER}/.cache/ccache"
    --env "CCACHE_MAXSIZE=${DEV_CCACHE_MAXSIZE:-10G}"
    --volume "${WORKSPACE_DIR}:${WORKSPACE_DIR}"
    --volume "${CCACHE_HOST_DIR}:/home/${DEV_USER}/.cache/ccache"
  )

  # Git worktree 的 .git 文件可能指向工作区外；额外挂载后容器内 git 可正常使用。
  common_dir="$(git_common_dir)"
  case "${common_dir}" in
    ""|"${WORKSPACE_DIR}"|"${WORKSPACE_DIR}"/*) ;;
    *) run_args+=(--volume "${common_dir}:${common_dir}") ;;
  esac

  # Linux 宿主机也可以用 host.docker.internal 访问宿主服务。
  run_args+=(--add-host "host.docker.internal:host-gateway" "${IMAGE_NAME}")
  docker "${run_args[@]}" >/dev/null
  log "容器已启动：${CONTAINER_NAME}"
}

start_container() {
  require_docker
  if container_exists && image_exists && ! container_uses_current_image; then
    die "容器使用的是旧镜像；请运行 ./env.sh rebuild 重新创建容器"
  fi
  if container_running; then
    log "容器已在运行：${CONTAINER_NAME}"
  elif container_exists; then
    docker start "${CONTAINER_NAME}" >/dev/null
    log "容器已启动：${CONTAINER_NAME}"
  else
    create_container
  fi
}

ensure_running() {
  if container_exists && image_exists && ! container_uses_current_image; then
    die "容器使用的是旧镜像；请运行 ./env.sh rebuild 重新创建容器"
  fi
  container_running || start_container
}

remove_container() {
  require_docker
  if container_exists; then
    docker rm --force "${CONTAINER_NAME}" >/dev/null
    log "容器已删除；工作区和 ccache 均已保留"
  else
    log "容器不存在：${CONTAINER_NAME}"
  fi
}

open_shell() {
  ensure_running
  docker exec \
    --interactive --tty \
    --env "TERM=${TERM:-xterm-256color}" \
    --env "COLORTERM=${COLORTERM:-truecolor}" \
    --workdir "${WORKSPACE_DIR}" \
    "${CONTAINER_NAME}" bash --login
}

exec_in_container() {
  ensure_running
  docker exec --workdir "${WORKSPACE_DIR}" "${CONTAINER_NAME}" "$@"
}

configure_brpc() {
  ensure_running
  log "配置 bRPC：${BUILD_DIR} (${BUILD_TYPE})"
  docker exec \
    --workdir "${WORKSPACE_DIR}" \
    "${CONTAINER_NAME}" \
    cmake \
      -S "${WORKSPACE_DIR}" \
      -B "${BUILD_DIR}" \
      -G Ninja \
      "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      "$@"
}

build_brpc() {
  local jobs

  ensure_running
  if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    configure_brpc
  fi

  jobs="${DEV_BUILD_JOBS:-$(docker exec "${CONTAINER_NAME}" nproc)}"
  [[ "${jobs}" =~ ^[1-9][0-9]*$ ]] || die "DEV_BUILD_JOBS 必须是正整数，当前值：${jobs}"

  log "编译 bRPC（${jobs} 个并行任务）"
  docker exec \
    --workdir "${WORKSPACE_DIR}" \
    "${CONTAINER_NAME}" \
    cmake --build "${BUILD_DIR}" --parallel "${jobs}"
}

compile_brpc() {
  configure_brpc "$@"
  build_brpc
}

compile_example() {
  local example_name="${1:-}"
  local example_source_dir
  local example_build_dir
  local jobs

  [[ -n "${example_name}" ]] || die "example 后需要提供目录名，例如：./env.sh example echo_c++"
  case "${example_name}" in
    .|..|*/*) die "example 目录名必须是 example/ 下的直接子目录：${example_name}" ;;
  esac
  shift

  example_source_dir="${WORKSPACE_DIR}/example/${example_name}"
  example_build_dir="${BUILD_DIR}/examples/${example_name}"
  [[ -d "${example_source_dir}" ]] || die "找不到 example 目录：${example_source_dir}"
  [[ -f "${example_source_dir}/CMakeLists.txt" ]] || \
    die "example 不支持 CMake 编译：${example_source_dir}"

  # example 独立于主工程配置；缺少 bRPC 产物时先编译主工程。
  if [[ ! -f "${BUILD_DIR}/output/include/brpc/server.h" ||
        ! -f "${BUILD_DIR}/output/lib/libbrpc.a" ]]; then
    log "未找到 bRPC 编译产物，先编译主工程"
    build_brpc
  else
    ensure_running
  fi

  jobs="${DEV_BUILD_JOBS:-$(docker exec "${CONTAINER_NAME}" nproc)}"
  [[ "${jobs}" =~ ^[1-9][0-9]*$ ]] || die "DEV_BUILD_JOBS 必须是正整数，当前值：${jobs}"

  log "配置 example/${example_name}：${example_build_dir} (${BUILD_TYPE})"
  docker exec \
    --workdir "${WORKSPACE_DIR}" \
    "${CONTAINER_NAME}" \
    cmake \
      -S "${example_source_dir}" \
      -B "${example_build_dir}" \
      -G Ninja \
      "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache \
      -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      "-DBRPC_INCLUDE_PATH=${BUILD_DIR}/output/include" \
      "-DBRPC_LIB=${BUILD_DIR}/output/lib/libbrpc.a" \
      "$@"

  log "编译 example/${example_name}（${jobs} 个并行任务）"
  docker exec \
    --workdir "${WORKSPACE_DIR}" \
    "${CONTAINER_NAME}" \
    cmake --build "${example_build_dir}" --parallel "${jobs}"
  log "example 编译完成：${example_build_dir}"
}

show_status() {
  local image_sync="n/a"

  require_docker
  if container_exists && image_exists; then
    image_sync="$(container_uses_current_image && printf yes || printf no)"
  fi
  printf 'container: %s\n' "${CONTAINER_NAME}"
  printf 'running:   %s\n' "$(container_running && printf yes || printf no)"
  printf 'image:     %s\n' "${IMAGE_NAME}"
  printf 'image sync: %s\n' "${image_sync}"
  printf 'platform:  %s\n' "${DOCKER_PLATFORM}"
  printf 'workspace: %s\n' "${WORKSPACE_DIR}"
  printf 'build dir:  %s\n' "${BUILD_DIR}"
  printf 'build type: %s\n' "${BUILD_TYPE}"
  printf 'ccache:    %s\n' "${CCACHE_HOST_DIR}"
}

usage() {
  cat <<'EOF'
用法：./env.sh <命令> [参数]

  up                  创建或启动开发容器
  shell               进入美化后的交互式 Bash
  exec <命令...>      在工作区执行非交互命令
  build [Docker 参数] 构建开发镜像（例如：build --no-cache）
  rebuild             删除容器、重建镜像并重新启动
  configure [CMake 参数]
                      配置 bRPC（默认使用 Ninja 和 ccache）
  compile [CMake 参数]
                      配置并编译 bRPC
  example <目录名> [CMake 参数]
                      编译 example/<目录名>，产物放在 build/examples/<目录名>
  status              显示当前配置和运行状态
  down                删除容器，保留源码和 ccache
  help                显示本帮助

可覆盖的环境变量：
  DEV_WORKSPACE_DIR, DEV_IMAGE_NAME, DEV_CONTAINER_NAME,
  DEV_CONTAINER_USER, DEV_DOCKER_PLATFORM, DEV_CCACHE_DIR,
  DEV_CCACHE_MAXSIZE, DEV_BUILD_DIR, DEV_BUILD_TYPE,
  DEV_BUILD_JOBS
EOF
}

command_name="${1:-shell}"
if [[ $# -gt 0 ]]; then
  shift
fi

case "${command_name}" in
  up)
    start_container
    ;;
  shell)
    open_shell
    ;;
  exec)
    [[ $# -gt 0 ]] || die "exec 后需要提供命令"
    exec_in_container "$@"
    ;;
  build)
    build_image "$@"
    ;;
  rebuild)
    remove_container
    build_image
    create_container
    ;;
  configure)
    configure_brpc "$@"
    ;;
  compile)
    compile_brpc "$@"
    ;;
  example)
    compile_example "$@"
    ;;
  status)
    show_status
    ;;
  down)
    remove_container
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    usage >&2
    die "未知命令：${command_name}"
    ;;
esac
