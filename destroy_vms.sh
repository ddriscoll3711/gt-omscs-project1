#! /bin/bash
# Script to create some VMs for test purposes
numvms=$1
for ((i=0;i<numvms;i++));do
echo Destroying vm$i...
uvt-kvm destroy vm$i 2> /dev/null
done
virsh list


