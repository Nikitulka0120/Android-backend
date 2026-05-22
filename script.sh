git submodule add https://github.com/epezent/implot.git third_party/implot
git submodule add -b docking https://github.com/ocornut/imgui.git third_party/imgui
git submodule update --init --recursive
sudo apt-get update
sudo apt-get install x11-server-utils
xhost +local:docker
docker-compose up -d --build