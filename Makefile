CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O3 -Wall -Wextra
LDLIBS    = -lcurl -lsolv -lsolvext -lpthread
TARGET1   = gw
TARGET2   = gwins
SRC       = gwins12.cpp
PREFIX   ?= /usr/local

all: $(TARGET1) $(TARGET2)

$(TARGET1): $(SRC)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

$(TARGET2): $(TARGET1)
	cp $(TARGET1) $(TARGET2)

install: all
	install -Dm755 $(TARGET1) $(DESTDIR)$(PREFIX)/bin/$(TARGET1)
	install -Dm755 $(TARGET2) $(DESTDIR)$(PREFIX)/bin/$(TARGET2)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET1)
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET2)

test: all
	./$(TARGET1) version
	./$(TARGET1) help
	./$(TARGET1) search -q openssl | head -5
	./$(TARGET1) install curl --pretend
	./$(TARGET2) version

clean:
	rm -f $(TARGET1) $(TARGET2)

.PHONY: all install uninstall test clean