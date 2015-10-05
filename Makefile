CFLAGS := -Wall -Wextra -g -fopenmp
LIBS   := -ltiff -lm -lgsl -lcblas

all : median_stacker

clean :
	rm -f median_stacker

median_stacker : median.c
	${CC} ${CFLAGS} -o $@ $^ ${LIBS}
