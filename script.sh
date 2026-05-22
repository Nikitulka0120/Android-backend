set -e
if [ ! -d "third_party/imgui" ]; then
    git clone -b docking https://github.com/ocornut/imgui.git third_party/imgui
fi

if [ ! -d "third_party/implot" ]; then
    git clone https://github.com/epezent/implot.git third_party/implot
fi
sudo apt-get update
sudo apt-get install -y x11-xserver-utils
xhost +local:docker
docker-compose up -d --build