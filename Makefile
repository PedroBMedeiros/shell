CC = gcc
CFLAGS = -Wall -pedantic -g

mush2: mush2.o
	gcc -L ~pn-cs357/Given/Mush/lib64 -o mush2 mush2.o -lmush
mush2.o: mush2.c
	gcc -c -I ~pn-cs357/Given/Mush/include -o mush2.o mush2.c
all: mush2
test: mush2
	./mush2
clean:
	rm *.o
