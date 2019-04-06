LIBS=$(shell pkg-config --libs zlib alsa) -lm -lpthread

ipoa: ipoa.c
	gcc ${CFLAGS} $^ -o $@ ${LIBS}
clean:
	rm -f ipoa
