#!/bin/bash

GREEN='\033[32m'
RED='\033[31m'
NC='\033[0m'

# Test 1: Program prints error message in response to
# invocation error

FILENAME=./ashti

# Define the sample files to use as input
FILES=""

# Define the options to pass to the program
OPTIONS=""

# Define the expected output
# Test may fail if test script ran from non project root directory
EXPECTED_OUTPUT="Usage: $FILENAME server_root"

# Run the program with the sample files and options
$FILENAME $OPTIONS ${FILES[@]} 2> output.txt

# Expected: Program exits with code 1 for INVOCATION_ERROR
# and outputs usage statement
if grep -q "$EXPECTED_OUTPUT" output.txt; then
    echo -e "1. Invocation Error Test             : ${GREEN}PASS${NC}"
else
    echo -e "1. Invocation Error Test             : ${RED}FAIL${NC}"
fi

################## SERVER SETUP ##################
# Background fdr process
$FILENAME test/my_server >/dev/null &
# Give server a moment to set up
sleep 1
##################################################

# Test 2: program connects and grabs basic sample text file
FILES=""
OPTIONS=""
EXPECTED_OUTPUT="This file has some content. c:"

# Expected: Program HTTP headers and the file content
echo -n "GET /sample.txt" | nc -w1 localhost $UID > output.txt

if grep -q "$EXPECTED_OUTPUT" output.txt; then
    echo -e "2. Basic Connection Test             : ${GREEN}PASS${NC}"
else
    echo -e "2. Basic Connection Test             : ${RED}FAIL${NC}"
fi

# Test 3: program connects and grabs basic sample image to
# demonstrate non .txt/.html extensions are supported
FILES=""
OPTIONS=""
EXPECTED_OUTPUT="Content-Type: image/jpeg
Content-Length: 126207"

# Expected: Program HTTP headers and the file content
echo -n "GET /sample.jpg" | nc -w1 localhost $UID > output.txt

if grep -q "$EXPECTED_OUTPUT" output.txt; then
    echo -e "3. .jpg Retrieval Test               : ${GREEN}PASS${NC}"
else
    echo -e "3. .jpg Retrieval Test               : ${RED}FAIL${NC}"
fi

# Test 4: program connects using HEAD method to get just the header
# for a file in /www
FILES=""
OPTIONS=""
EXPECTED_OUTPUT="Content-Type: image/jpeg
Content-Length: 126207"

# Expected: Program HTTP headers and the file content
echo -n "HEAD sample.jpg" | nc -w1 localhost $UID > output.txt

if grep -q "$EXPECTED_OUTPUT" output.txt; then
    echo -e "4. HEAD Functionality Test           : ${GREEN}PASS${NC}"
else
    echo -e "4. HEAD Functionality Test           : ${RED}FAIL${NC}"
fi

# Test 5: program returns a 400 Bad Request when given a bad request
FILES=""
OPTIONS=""
EXPECTED_OUTPUT="HTTP/1.1 400 Bad Request"

# Expected: Program HTTP headers and the file content
echo -n "GE /sample.jpg" | nc -w1 localhost $UID > output.txt

if grep -q "$EXPECTED_OUTPUT" output.txt; then
    echo -e "5. Bad Request Test                  : ${GREEN}PASS${NC}"
else
    echo -e "5. Bad Request Test                  : ${RED}FAIL${NC}"
fi

# Test 6: program returns a 400 Bad Request when given a request
# for a non-existent resource
FILES=""
OPTIONS=""
EXPECTED_OUTPUT="HTTP/1.1 404 Not Found"

# Expected: Program HTTP headers and the file content
echo -n "GET /sample.png" | nc -w1 localhost $UID > output.txt

if grep -q "$EXPECTED_OUTPUT" output.txt; then
    echo -e "6. 404 Error Test                    : ${GREEN}PASS${NC}"
else
    echo -e "6. 404 Error Test                    : ${RED}FAIL${NC}"
fi

# Test 7a: program returns a 403 Forbidden when given a request
# for an illegal resource (out of bounds of server root)
FILES=""
OPTIONS=""
EXPECTED_OUTPUT="HTTP/1.1 403 Forbidden"

# Expected: Program HTTP headers and the file content
echo -n "GET /../../../makefile" | nc -w1 localhost $UID > output.txt

if grep -q "$EXPECTED_OUTPUT" output.txt; then
    echo -e "7a. 403 Error Test                   : ${GREEN}PASS${NC}"
else
    echo -e "7a. 403 Error Test                   : ${RED}FAIL${NC}"
fi

# Test 7b: program returns a 403 Forbidden when given a request
# for an illegal resource (no permissions)

chmod -rwx test/my_server/www/unreachable.jpg

FILES=""
OPTIONS=""
EXPECTED_OUTPUT="HTTP/1.1 403 Forbidden"

# Expected: Program HTTP headers and the file content
echo -n "GET unreachable.jpg" | nc -w1 localhost $UID > output.txt

if grep -q "$EXPECTED_OUTPUT" output.txt; then
    echo -e "7b. 403 Error Test                   : ${GREEN}PASS${NC}"
else
    echo -e "7b. 403 Error Test                   : ${RED}FAIL${NC}"
fi


#################### CLEANUP ####################
# Delete temp file
#rm output.txt

# Close backgrounded server process
pkill ashti
#################################################

