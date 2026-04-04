
```sh
# open terminal to send stuff to arduino via UART
screen /dev/cu.usbmodem11101 115200
# challenge <tt> <bt> <tc> <bc> <count> [show_summary]
debug_on
challenge choice-puf 1 6 0 7 100 2
challenge choice-puf 2 0 0 2 100 1
challenge choice-puf 3 0 1 4 100 1


find_valid
reconfigure
challenge 1234333
# (303826966) (303826966)
```