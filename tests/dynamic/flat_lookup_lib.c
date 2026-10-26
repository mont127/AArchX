/* flat_lookup's dylib, which the program links: it defines what the bundle imports flat, xmlCheckVersion among it,
   a name libxml2 in the shared cache exports too, though nothing here loads libxml2. */
int flat_seen;

int flat_answer(void)
{
    return 42;
}

void xmlCheckVersion(int version)
{
    flat_seen = version;
}
