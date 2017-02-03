#!/bin/bash

cat command-lines.in | grep -v '^#' | grep -v '\-\-\-' | grep -v '^$' > command-lines.tmp
echo "" >> command-lines.tmp
echo "const char _command_input[] =" > command-lines-input.h
while read -r line; do
	echo "" >> command-lines-input.h
	printf '\"%s\\n\"' "$line" >> command-lines-input.h
done < command-lines.tmp
echo ";" >> command-lines-input.h

