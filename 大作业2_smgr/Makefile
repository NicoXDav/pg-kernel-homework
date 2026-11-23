# 大作业二：smgr 接口 + 本地块设备存储管理器
# 作者：孙翌程  学号：2601210437

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11
TARGET  = test_smgr

OBJS = smgr_blockdevice.o test_smgr.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c smgr_blockdevice.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET) disk.img

clean:
	rm -f $(TARGET) *.o disk.img

.PHONY: all run clean
