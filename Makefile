# ============================================================
# 本地编译（x86_64，WSL 快速验证用）
# ============================================================
CC_LOCAL    = gcc
CFLAGS_LOCAL = -Wall -Wextra -O2 -g -pthread -U_FORTIFY_SOURCE
LDFLAGS_LOCAL = -lpthread
TARGET       = tiny-httpd
# Day2 阶段: 仅 main/server/http_parser
# Day4 加入: src/logger.c, Day5 加入: src/gui.c
LOCAL_SRCS   = src/main.c src/server.c src/http_parser.c

OBJS = $(LOCAL_SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC_LOCAL) $(CFLAGS_LOCAL) -o $@ $^ $(LDFLAGS_LOCAL)

%.o: %.c
	$(CC_LOCAL) $(CFLAGS_LOCAL) -c -o $@ $<

# ============================================================
# ARM 交叉编译（部署到开发板）
# 编译器: arm-linux-gcc 5.4.0（32位 ARM）
# ============================================================
CROSS_PREFIX = /home/wz/usr/local/arm/5.4.0/usr/bin/arm-linux-
CC_ARM       = $(CROSS_PREFIX)gcc
CXX_ARM      = $(CROSS_PREFIX)g++

ARM_SRCS     = $(LOCAL_SRCS) src/gui.c

ARM_CFLAGS   = -Wall -Wextra -O2 -g -pthread \
               -I/home/wz/usr/local/openssl-arm/include

ARM_LIBS     = -lpthread -lm -lrt -ldl

arm:
	$(CC_ARM) $(ARM_CFLAGS) -o $(TARGET) $(ARM_SRCS) $(ARM_LIBS)
	file $(TARGET)

# ARM 模式（LVGL 源码内联编译，如果 LVGL 在其他目录需修改 LVGL_PATH）
LVGL_PATH    = /home/wz/codepath/lvgl_fb
ARM_ALL_SRCS = $(ARM_SRCS)
ARM_ALL_SRCS += $(wildcard $(LVGL_PATH)/lvgl/src/*.c $(LVGL_PATH)/lvgl/src/**/*.c)
ARM_ALL_SRCS += $(wildcard $(LVGL_PATH)/lv_drivers/*.c $(LVGL_PATH)/lv_drivers/**/*.c)
ARM_CFLAGS_LVGL = $(ARM_CFLAGS) -I$(LVGL_PATH) -I$(LVGL_PATH)/lvgl -I$(LVGL_PATH)/lv_drivers
ARM_LIBS_LVGL   = $(ARM_LIBS) -lssl -lcrypto

arm-full:
	$(CC_ARM) $(ARM_CFLAGS_LVGL) -o $(TARGET) $(ARM_ALL_SRCS) $(ARM_LIBS_LVGL)
	file $(TARGET)

# ============================================================
# 部署（请修改为实际开发板 IP）
# ============================================================
BOARD_IP   = 192.168.1.100
BOARD_DIR  = /root

deploy: arm
	scp $(TARGET) root@$(BOARD_IP):$(BOARD_DIR)/
	scp -r www root@$(BOARD_IP):$(BOARD_DIR)/

# ============================================================
# 工具目标
# ============================================================
clean:
	rm -f $(TARGET) src/*.o

run: $(TARGET)
	./$(TARGET) 8080

debug: CFLAGS_LOCAL += -DDEBUG -O0 -g
debug: $(TARGET)

test: CFLAGS_LOCAL += -DTEST_MODE -DDEBUG -O0 -g
test: $(TARGET)

stress: test/stress.c
	$(CC_LOCAL) -Wall -O2 -o stress_test test/stress.c -lpthread

bench: $(TARGET) stress
	./$(TARGET) -p 8080 -t 4 &
	sleep 1
	./stress_test -h 127.0.0.1 -p 8080 -n 5000 -c 50
	kill %1

# 无 LVGL 的 ARM 编译
headless:
	$(CC_ARM) $(ARM_CFLAGS) -DENABLE_LVGL=0 \
		-o $(TARGET) $(LOCAL_SRCS) $(ARM_LIBS)

.PHONY: arm arm-full deploy clean run debug test stress bench headless
