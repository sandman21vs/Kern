CC = gcc
CFLAGS = -Wall -Wextra -g -I../src

SRCS_RECORD = test_nfc_record.c ../src/nfc_record.c
TARGET_RECORD = test_nfc_record

all: $(TARGET_RECORD)

$(TARGET_RECORD): $(SRCS_RECORD)
	$(CC) $(CFLAGS) -o $@ $^

run: $(TARGET_RECORD)
	./$(TARGET_RECORD)

clean:
	rm -f $(TARGET_RECORD)

.PHONY: all run clean
