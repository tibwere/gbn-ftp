#!/bin/sh
GBN_FOLDER_PATH="${HOME}/.gbn-ftp-public/";
TMP_LS_FILE="${HOME}/.gbn-ftp-public/.tmp-ls";
README_FILE="${HOME}/.gbn-ftp-public/README.txt";

if [ -d $GBN_FOLDER_PATH ]
then
        echo -ne "Installation folder already exists\n\nDo you want to overwrite it? [y/n] "
        read answer

        if [[ $answer == 'Y' || $answer == 'y' ]]
        then
                rm -rf $GBN_FOLDER_PATH
                mkdir -p $GBN_FOLDER_PATH;
                echo "Hello world!" > $README_FILE;
                ls $GBN_FOLDER_PATH > $TMP_LS_FILE; 
                echo -e "\nServer succesfully installed!"
        else
                echo -e "\nServer not installed"
        fi
else
        mkdir -p $GBN_FOLDER_PATH;
        echo "Hello world!" > $README_FILE;
        ls $GBN_FOLDER_PATH > $TMP_LS_FILE; 

        echo "Server succesfully installed!"
fi


