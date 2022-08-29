# Self-Documented Makefile
# https://marmelab.com/blog/2016/02/29/auto-documented-makefile.html
.PHONY: help
help:
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*?## "}; {printf "\033[36m%-20s\033[0m %s\n", $$1, $$2}'

NVDS_VERSION:=6.0

GST_PLUGIN_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/sources/gst-plugins
OSDCOORD_DIR?=/opt/nvidia/deepstream/deepstream-$(NVDS_VERSION)/sources/gst-plugins/gst-dsosdcoordrmq
CONFIG_FILE_PATH=/opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_infer_primary.txt

PWD=$(shell pwd)
DIR_NAME=rabbitmq-c
DIR_EXISTS="$(shell ls ${PWD} | grep ${DIR_NAME})"

install: ## jansson, rabbitmq-cのインストール
	sudo apt-get install -y libjansson-dev libssl-dev

ifeq ("$(shell echo ${DIR_EXISTS})", "$(shell echo ${DIR_NAME})")
	@echo "Destination path 'rabbitmq-c' already exists!"
else
	git clone https://github.com/alanxz/rabbitmq-c.git && mkdir -p rabbitmq-c/build
	cd rabbitmq-c/build; cmake .. && sudo cmake --build . --target install
endif

build: ## dsosdcoordrmqのビルド
	sudo cp -r gst-dsosdcoordrmq $(GST_PLUGIN_DIR)
	sudo make -C $(OSDCOORD_DIR)
	sudo make -C $(OSDCOORD_DIR) install

start: ## ストリームの開始
	gst-launch-1.0 \
    		-e \
    		v4l2src ! video/x-raw,width=800,height=448,framerate=30/1,format=YUY2 ! \
    		nvvideoconvert ! 'video/x-raw(memory:NVMM),format=NV12' ! \
    		m.sink_0 nvstreammux name=m width=800 height=448 batch_size=1 ! \
    		nvinfer config-file-path=$(CONFIG_FILE_PATH) ! \
    		nvvideoconvert ! 'video/x-raw(memory:NVMM),format=RGBA' ! \
		dsosdcoordrmq ! nvegltransform ! nveglglessink sync=0
