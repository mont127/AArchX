/* flat_lookup's bundle, linked -undefined dynamic_lookup: every import is a flat lookup. */
int flat_answer(void);
void xmlCheckVersion(int version);
extern int flat_seen;

int flat_entry(void)
{
    xmlCheckVersion(7);
    return flat_answer() * 100 + flat_seen;
}
