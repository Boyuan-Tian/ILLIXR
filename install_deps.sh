#!/bin/bash

### Extra instructions for docker ###
# export DEBIAN_FRONTEND=noninteractive TZ=America/Chicago
# apt update && apt install -y sudo

### Normalize environment ###

set -e
cd "$(dirname "${0}")"

### Parse args ###

assume_yes=
while [[ "$#" -gt 0 ]]; do
    case "${1}" in
        -y|--yes) assume_yes=true ;;
        *) echo "Unknown parameter passed: ${1}"; exit 1 ;;
    esac
    shift
done

### Get OS ###

. /etc/os-release

### Helper functions ###

function y_or_n() {
	if [ -n "${assume_yes}" ]; then
		return 0
	else
		while true; do
			echo "${1}"
			read -rp "Yes or no? " yn
			case "${yn}" in
				[Yy]* ) return 0 ;;
				[Nn]* ) echo "Declined."; return 1 ;;
				* ) echo "Please answer yes or no." ;;
			esac
		done
	fi
}

### Main ###

if [ "${ID_LIKE}" = debian ] || [ "${ID}" = debian ]
then
	if y_or_n "Next: Add apt-get sources list and keys"; then
		sudo apt install -y software-properties-common curl
		curl https://apt.kitware.com/keys/kitware-archive-latest.asc | sudo apt-key add -
		sudo apt-add-repository -u -y "deb https://apt.kitware.com/ubuntu/ ${UBUNTU_CODENAME} main"
		sudo add-apt-repository -u -y ppa:graphics-drivers/ppa
		sudo add-apt-repository -u -y ppa:deadsnakes/ppa
	fi

	if y_or_n "Next: apt-get install necessary packages"; then
		sudo apt-get install -y \
			git clang make cmake libc++-dev libc++abi-dev python3.8 python3-venv \
			libeigen3-dev libboost-all-dev libatlas-base-dev libsuitesparse-dev libblas-dev \
			glslang-tools libsdl2-dev libglu1-mesa-dev mesa-common-dev freeglut3-dev libglew-dev glew-utils libglfw3-dev \
			libusb-dev libusb-1.0 libudev-dev libv4l-dev libhidapi-dev \
			build-essential libx11-xcb-dev libxcb-glx0-dev libxkbcommon-dev libwayland-dev libxrandr-dev \
			libgtest-dev pkg-config libgtk2.0-dev
	fi

	temp_dir=/tmp/ILLIXR_deps
	mkdir -p "${temp_dir}"

	if [ ! -d opencv ] && y_or_n "Next: Install OpenCV from source"; then
		git clone --branch 3.4.6 https://github.com/opencv/opencv/ "${temp_dir}/opencv"
		git clone --branch 3.4.6 https://github.com/opencv/opencv_contrib/  "${temp_dir}/opencv_contrib"
		cmake \
			-S "${temp_dir}/opencv" \
			-B "${temp_dir}/opencv/build" \
			-D CMAKE_BUILD_TYPE=Release \
			-D CMAKE_INSTALL_PREFIX=/usr/local \
			-D BUILD_TESTS=OFF \
			-D BUILD_PERF_TESTS=OFF \
			-D BUILD_EXAMPLES=OFF \
			-D BUILD_JAVA=OFF \
			-D WITH_OPENGL=ON \
			-D OPENCV_EXTRA_MODULES_PATH="${temp_dir}/opencv_contrib/modules"
		sudo make -C "${temp_dir}/opencv/build" "-j$(nproc)" install
		sudo ldconfig -v
	fi

	if [ ! -d Vulkan-Headers ] && y_or_n "Next: Install OpenCV from source"; then
		git clone https://github.com/KhronosGroup/Vulkan-Headers.git "${temp_dir}/Vulkan-Headers"
		cmake \
			-S "${temp_dir}/Vulkan-Headers" \
			-B "${temp_dir}/Vulkan-Headers/build" \
			-D CMAKE_INSTALL_PREFIX=install
		sudo make -C "${temp_dir}/Vulkan-Headers/build" "-j$(nproc)" install
	fi

	# if [ ! -d Vulkan-Loader ]; then
	# 	echo "Next: Install Vulkan Loader from source"
	# 	if y_or_n; then
	# 		git clone https://github.com/KhronosGroup/Vulkan-Loader.git
	# 		mkdir -p Vulkan-Headers/build && cd Vulkan-Headers/build
	# 		../scripts/update_deps.py
	# 		cmake -C helper.cmake ..
	# 		cmake --build .
	# 		cd ../..
	# 	fi
	# fi

	if [ ! -d OpenXR-SDK ] && y_or_n "Next: Install OpenXR SDK from souce"; then
		git clone https://github.com/KhronosGroup/OpenXR-SDK.git "${temp_dir}/OpenXR-SDK"
		cmake -S "${temp_dir}/OpenXR-SDK" -B "${temp_dir}/OpenXR-SDK/build"
		sudo make -C "${temp_dir}/OpenXR-SDK/build" "-j$(nproc)" install
	fi

	if ! poetry; then
		if y_or_n "Next: Install Poetry"; then
			curl -sSL https://raw.githubusercontent.com/python-poetry/poetry/master/get-poetry.py | python3.8
			. $HOME/.poetry/env
		fi
	fi

	# I won't ask the user first, because this is not a global installation.
	# All of this stuff goes into a project-specific venv.
	cd runner
	poetry install
	cd ..
else
	echo "${0} does not support ${ID_LIKE} yet."
	exit 1
fi
