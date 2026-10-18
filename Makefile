ifdef PS5_PAYLOAD_SDK
  include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
  $(error PS5_PAYLOAD_SDK is undefined)
endif

CFLAGS := -O2 -Wall -Isrc
SRCS   := src/main.c src/telemetry.c

all: telemetry.elf

src/dashboard.h: web/index.html tools/embed.py
	python3 tools/embed.py web/index.html src/dashboard.h

telemetry.elf: $(SRCS) src/dashboard.h src/telemetry.h
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDADD)

clean:
	rm -f telemetry.elf src/dashboard.h
