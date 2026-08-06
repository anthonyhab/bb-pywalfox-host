CC ?= cc
# -D_FORTIFY_SOURCE=3 assumes glibc >= 2.35 (Arch's baseline since March 2022);
# use =2 for older glibc.
CFLAGS ?= -O2 -pipe
CFLAGS += -fPIE -fstack-protector-strong -Wformat=2 -Wformat-truncation \
          -D_FORTIFY_SOURCE=3
# PIE + full RELRO + noexec stack; -fPIE above makes the objects PIE-ready.
LDFLAGS += -fPIE -pie -Wl,-z,relro,-z,now,-z,noexecstack
LDLIBS ?= -ljson-c
WARNINGS := -Wall -Wextra -Wpedantic
TARGET := bb-pywalfox-host
SOURCES := bb-pywalfox-host.c css_manager.c
ASSETS := assets/userChrome.css assets/userContent.css

.PHONY: all clean test sanitize analyze ci

all: $(TARGET)

$(TARGET): $(SOURCES) assets_embedded.h css_manager.h bb-common/native_messaging.h bb-common/json_utils.h bb-common/config_utils.h
	$(CC) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

# CI-only: treat every warning as an error.
ci: CFLAGS += -Werror
ci: all

# GCC 12+ static analyzer pass over both translation units.
analyze:
	$(CC) $(CFLAGS) $(WARNINGS) -std=c11 -fanalyzer -o $(TARGET)-analyzer $(SOURCES) $(LDLIBS)
	rm -f $(TARGET)-analyzer

test: $(TARGET)
	HOST_BINARY="$(CURDIR)/$(TARGET)" python3 tests/test_protocol.py

sanitize:
	$(CC) -O1 -g $(WARNINGS) -std=c11 -fsanitize=address,undefined -fno-omit-frame-pointer -o $(TARGET)-sanitize $(SOURCES) $(LDLIBS)
	HOST_BINARY="$(CURDIR)/$(TARGET)-sanitize" python3 tests/test_protocol.py
	rm -f $(TARGET)-sanitize

clean:
	rm -f $(TARGET) $(TARGET)-sanitize $(TARGET)-analyzer
