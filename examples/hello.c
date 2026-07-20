int sum_to(int n) {
    int total = 0;
    int i;
    for (i = 1; i <= n; i = i + 1)
        total = total + i;
    return total;
}

int main(int argc, char **argv) {
    int answer = sum_to(9);
    if (answer != 45)
        return 99;
    if (argc < 1 || argv == 0)
        return 98;
    puts("bcc-hello-ok");
    return 42;
}
