# 课1-2 开发分析题：链表搜索 + perf_event_open 统计
# 作者：孙翌程  学号：2601210437

CC      = gcc
CFLAGS  = -Wall -Wextra -O2
TARGET  = linked_search_perf

all: $(TARGET)

$(TARGET): linked_search_perf.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
