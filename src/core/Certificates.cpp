#include "mail/Certificates.h"
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <memory>
#include <stdexcept>

namespace mail {
namespace {
void require(bool ok,const char* why) { if(!ok) throw std::invalid_argument(why); }
using Bio=std::unique_ptr<BIO,decltype(&BIO_free)>;
using Certificate=std::unique_ptr<X509,decltype(&X509_free)>;
std::string read(BIO* bio)
{
    char* data=nullptr; auto size=BIO_get_mem_data(bio,&data);
    return size>0?std::string(data,static_cast<std::size_t>(size)):std::string();
}
std::string name(X509_NAME* value)
{
    Bio bio(BIO_new(BIO_s_mem()),BIO_free); require(bool(bio),"Out of memory.");
    X509_NAME_print_ex(bio.get(),value,0,XN_FLAG_ONELINE & ~ASN1_STRFLGS_ESC_MSB); return read(bio.get());
}
std::string time(const ASN1_TIME* value)
{
    Bio bio(BIO_new(BIO_s_mem()),BIO_free); require(bool(bio),"Out of memory.");
    ASN1_TIME_print(bio.get(),value); return read(bio.get());
}
}
bool isPemText(const std::string& value)
{
    auto start=value.find_first_not_of(" \t\r\n");
    return start!=std::string::npos && value.compare(start,11,"-----BEGIN ")==0;
}
std::string normalizePem(const std::string& text)
{
    require(text.size()<=1024*1024,"The certificate text is too large.");
    std::string result; std::size_t at=0;
    for(;;) {
        auto begin=text.find("-----BEGIN ",at); if(begin==std::string::npos) break;
        auto labelEnd=text.find("-----",begin+11); require(labelEnd!=std::string::npos,"The certificate text is incomplete.");
        auto label=text.substr(begin+11,labelEnd-begin-11);
        auto footer="-----END "+label+"-----"; auto end=text.find(footer,labelEnd+5);
        require(end!=std::string::npos,"The certificate text is missing its END line.");
        at=end+footer.size();
        if(label!="CERTIFICATE" && label!="TRUSTED CERTIFICATE") continue;
        std::string body;
        for(auto i=labelEnd+5;i<end;++i) {
            unsigned char c=text[i];
            if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='+'||c=='/'||c=='=') body+=static_cast<char>(c);
            else require(c==' '||c=='\t'||c=='\r'||c=='\n',"The certificate text contains invalid characters.");
        }
        require(!body.empty(),"The certificate text is empty.");
        result+="-----BEGIN "+label+"-----\n";
        for(std::size_t i=0;i<body.size();i+=64) result+=body.substr(i,64)+'\n';
        result+=footer+'\n';
    }
    require(!result.empty(),"The text does not contain a PEM certificate.");
    return result;
}
CertificateDetails describeCertificate(const std::string& bytes)
{
    require(!bytes.empty() && bytes.size()<=1024*1024,"The certificate file is empty or too large.");
    Certificate certificate(nullptr,X509_free);
    if(isPemText(bytes)) {
        auto pem=normalizePem(bytes);
        Bio bio(BIO_new_mem_buf(pem.data(),static_cast<int>(pem.size())),BIO_free); require(bool(bio),"Out of memory.");
        certificate.reset(PEM_read_bio_X509_AUX(bio.get(),nullptr,nullptr,nullptr));
    } else {
        auto* data=reinterpret_cast<const unsigned char*>(bytes.data());
        certificate.reset(d2i_X509(nullptr,&data,static_cast<long>(bytes.size())));
    }
    require(bool(certificate),"This file is not a PEM or DER certificate.");
    CertificateDetails details;
    Bio pem(BIO_new(BIO_s_mem()),BIO_free); require(pem && PEM_write_bio_X509(pem.get(),certificate.get()),"Could not convert the certificate.");
    details.pem=read(pem.get());
    details.subject=name(X509_get_subject_name(certificate.get())); details.issuer=name(X509_get_issuer_name(certificate.get()));
    details.selfSigned=X509_NAME_cmp(X509_get_subject_name(certificate.get()),X509_get_issuer_name(certificate.get()))==0;
    details.validFrom=time(X509_get0_notBefore(certificate.get())); details.validUntil=time(X509_get0_notAfter(certificate.get()));
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned int length=0;
    require(X509_digest(certificate.get(),EVP_sha256(),digest,&length)==1,"Could not compute the certificate fingerprint.");
    static const char hex[]="0123456789ABCDEF";
    for(unsigned int i=0;i<length;++i) { if(i) details.sha256+=':'; details.sha256+=hex[digest[i]>>4]; details.sha256+=hex[digest[i]&15]; }
    return details;
}
}
