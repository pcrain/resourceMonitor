all:
	gcc -Wall -Ofast -s -DNDEBUG -march=native resourceMonitor.c -o resmon
	# clang -Ofast -s -DNDEBUG -march=native resourceMonitor.c -o resmon

debug:
	gcc -Wall -O3 resourceMonitor.c -o resmon

clean:
	rm -f ./resmon
