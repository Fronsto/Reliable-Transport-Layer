#!/bin/sh
cp ./reliable.c ./code/
cp ./reliable.c ./Testing/
cd ./code
echo "running make on /code"
make
cd ../Testing
echo ""
echo "running make on /Testing"
make