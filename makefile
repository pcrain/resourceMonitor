all:
	gcc -O3 resourceMonitor.c -o resmon
	# gcc -O3 -s -DNDEBUG resourceMonitor.c -o resmon

clean:
	rm ./resmon
