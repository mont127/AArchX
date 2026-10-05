#include <Kerberos/gssapi.h>
#include <Kerberos/gssapi_krb5.h>
#include <stdio.h>
static void show(const char *tag, const gss_OID_desc *o)
{
    printf("%s len=%u", tag, o ? o->length : 0);
    for (OM_uint32 k = 0; o && k < o->length; k++)
        printf(" %02x", ((const unsigned char *)o->elements)[k]);
    printf("\n");
}
int main(void)
{
    show("nt_user", GSS_C_NT_USER_NAME);
    show("nt_hostbased", GSS_C_NT_HOSTBASED_SERVICE);
    show("mech_krb5", gss_mech_krb5);
    printf("set count=%zu\n", gss_mech_set_krb5 ? gss_mech_set_krb5->count : 0);
    for (size_t k = 0; gss_mech_set_krb5 && k < gss_mech_set_krb5->count; k++)
        show("set member", &gss_mech_set_krb5->elements[k]);
    return 0;
}
