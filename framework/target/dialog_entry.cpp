// Where the game calls the set-up of a dialog's widget, the request it sets it
// up from is in a register that is no argument. This hands it on as a fourth
// argument, which the set-up does not use, and goes on to the framework's function.
// Which register is per build, so there is one entry for each, in order.

extern "C" void tsuchi_dialog_on_setup(void* widget, unsigned kind, int layout, void* request);

extern "C" __attribute__((naked)) void tsuchi_dialog_setup_entries() {
    __asm__(".irp number, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30\n"
            "mov x3, x\\number\n"
            "b tsuchi_dialog_on_setup\n"
            ".endr\n");
}
