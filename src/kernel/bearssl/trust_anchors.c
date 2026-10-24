#include "bearssl.h"
#include "trust_anchors.inc"

const br_x509_trust_anchor *aether_tls_trust_anchors(void)
{
	return TAs;
}

size_t aether_tls_trust_anchors_num(void)
{
	return TAs_NUM;
}
