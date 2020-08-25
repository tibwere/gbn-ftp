#!/bin/sh
GBN_FOLDER_PATH="${HOME}/.gbn-ftp-download/"

if [ -d $GBN_FOLDER_PATH ]
then
        echo -ne "Installation folder already exists\n\nDo you want to overwrite it? [y/n] "
        read answer

        if [[ $answer == 'Y' || $answer == 'y' ]]
        then
                rm -rf $GBN_FOLDER_PATH
                mkdir -p $GBN_FOLDER_PATH
                echo -e "\nClient succesfully installed!"
        else
                echo -e "\nClient not installed"
        fi
else
        mkdir -p $GBN_FOLDER_PATH
        echo "Client succesfully installed!" 
fi

