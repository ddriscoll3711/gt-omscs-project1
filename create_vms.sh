#! /bin/bash
# Script to create some VMs for test purposes
numvms=$1
for ((i=0;i<numvms;i++));do
echo Creating vm$i...
uvt-kvm create vm$i release=xenial arch=amd64 2> /dev/null
done
virsh list


