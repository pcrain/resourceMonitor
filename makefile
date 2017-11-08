all:
	gcc -Ofast -s -DNDEBUG resourceMonitor.c -o resmon

debug:
	gcc -O3 resourceMonitor.c -o resmon

clean:
	rm ./resmon
