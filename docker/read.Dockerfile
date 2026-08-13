FROM ubuntu:24.04

ARG DEV_USER=developer
ARG DEV_UID=1000
ARG DEV_GID=1000

ENV DEBIAN_FRONTEND=noninteractive \
    LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    HOME=/home/${DEV_USER} \
    USER=${DEV_USER} \
    LOGNAME=${DEV_USER} \
    CCACHE_DIR=/home/${DEV_USER}/.cache/ccache \
    CCACHE_MAXSIZE=10G

# 构建工具 + 一组轻量的交互式终端工具：
# bash-completion（补全）、fzf（模糊查找）、zoxide（智能跳转）、
# bat/fd/ripgrep（更友好的查看与搜索）以及 tmux。
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        bash \
        bash-completion \
        bat \
        build-essential \
        ca-certificates \
        ccache \
        clang \
        clang-format \
        clangd \
        cmake \
        curl \
        fd-find \
        fzf \
        gdb \
        git \
        jq \
        less \
        libgflags-dev \
        libleveldb-dev \
        libprotobuf-dev \
        libprotoc-dev \
        libsnappy-dev \
        libssl-dev \
        ninja-build \
        openssh-client \
        pkg-config \
        protobuf-compiler \
        python3 \
        python3-dev \
        python3-pip \
        python3-protobuf \
        python3-venv \
        ripgrep \
        sudo \
        tmux \
        tree \
        vim \
        zlib1g-dev \
        zoxide \
    && rm -rf /var/lib/apt/lists/*

# 与宿主机 UID/GID 保持一致，避免容器生成 root 所有的文件。
RUN set -eux; \
    group_name="$(getent group "${DEV_GID}" | cut -d: -f1 || true)"; \
    if [ -z "${group_name}" ]; then \
        group_name="${DEV_USER}"; \
        groupadd --gid "${DEV_GID}" "${group_name}"; \
    fi; \
    useradd --create-home --shell /bin/bash --uid "${DEV_UID}" --gid "${group_name}" "${DEV_USER}"; \
    install -d -o "${DEV_UID}" -g "${DEV_GID}" "/home/${DEV_USER}/.cache/ccache"; \
    echo "${DEV_USER} ALL=(ALL) NOPASSWD:ALL" > "/etc/sudoers.d/${DEV_USER}"; \
    chmod 0440 "/etc/sudoers.d/${DEV_USER}"

COPY --chown=${DEV_UID}:${DEV_GID} bashrc.template /home/${DEV_USER}/.bashrc

USER ${DEV_USER}
WORKDIR /workspace

CMD ["sleep", "infinity"]
