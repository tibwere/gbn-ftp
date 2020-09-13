#!/bin/sh

# File..: test.sh
# Autore: Simone Tiberi (M. 0252795)
#
# Questo script di test è stato ideato per essere utilizzato solo
# nel caso in cui il server sia in esecuzione sulla stessa macchina
# del client. 
# 
# ATTENZIONE: Prima di lanciare lo script assicurarsi di aver:
#		1) ripulito la cartella d'installazione del client (tramite lo script dedicato)
#		2) lanciato il server gbn-ftp

echo "STEP 1: Download all files"
for f in $(/usr/bin/ls $HOME/.gbn-ftp-public/)
do
        echo -e "g\n$f\n\nq" > input.txt
        echo "Downloading $f ..."
        $PWD/../bin/gbn-ftp-client -a 127.0.0.1 --verbose < input.txt  > /dev/null 2>&1
done
rm -f input.txt
echo -e "\nAll files downloaded succesfully!"



echo -e "\n\nSTEP 2: Check for integrity of each file"
for f in $(/usr/bin/ls $HOME/.gbn-ftp-public/)
do
	if [ -f "$HOME/.gbn-ftp-download/$f" ]
	then
		echo -n "File: $f -> Test result: "
		if [ $(diff "$HOME/.gbn-ftp-download/$f" "$HOME/.gbn-ftp-public/$f") ]
		then
			echo "FAIL"
		else
			echo "SUCCESS"
		fi
	fi
done

