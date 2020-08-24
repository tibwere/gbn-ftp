#!/bin/sh
GBN_FOLDER_PATH="${HOME}/.gbn-ftp-public/";
TMP_LS_FILE="${HOME}/.gbn-ftp-public/.tmp-ls";
README_FILE="${HOME}/.gbn-ftp-public/README.txt";

mkdir -p $GBN_FOLDER_PATH;
echo "Hello world!" > $README_FILE;
ls $GBN_FOLDER_PATH > $TMP_LS_FILE; 

echo "Server succesfully installed!"