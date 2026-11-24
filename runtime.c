/* Current newlib expects this hook; Ndless performs its own startup/cleanup. */
void __attribute__((weak)) _fini(void)
{
}
