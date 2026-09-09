CC = gcc
CFLAGS = -Wall -Wextra -g -I.. -I../assets

SRCS = test_speech_text.c ../speech_text.c ../adpcm.c ../assets/speech_lexicon.c
TARGET = test_speech_text

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)

.PHONY: all run clean
