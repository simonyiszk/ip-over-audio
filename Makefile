ALSALIBS=$(shell pkg-config alsa --libs)

ipoa: ipoa.c
	gcc $^ -o $@ ${ALSALIBS} -lm -lpthread
clean:
	rm -f ipoa
