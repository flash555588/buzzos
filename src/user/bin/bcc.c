#include "libc.h"
#include "basm.h"

/* BuzzOS native C compiler. Frontend sections are kept in one translation
 * unit so the compiler can run without a host filesystem or dynamic linker. */

#define SRC_MAX 32768
#define ASM_MAX 32768
#define TOK_MAX 4096
#define NODE_MAX 3072
#define VAR_MAX 256
#define STR_MAX 128
#define NAME_MAX 32

enum { TK_EOF, TK_IDENT, TK_NUM, TK_STRING, TK_PUNCT };
enum {
    N_NUM, N_VAR, N_STRING, N_ADDR, N_DEREF, N_ASSIGN, N_NEG, N_NOT,
    N_BNOT, N_ADD, N_SUB, N_MUL, N_DIV, N_MOD, N_SHL, N_SHR, N_LT,
    N_LE, N_EQ, N_NE, N_AND, N_XOR, N_OR, N_LAND, N_LOR, N_CALL,
    N_EXPR, N_RETURN, N_IF, N_WHILE, N_FOR, N_BLOCK, N_BREAK, N_CONTINUE
};
typedef struct Token Token; typedef struct Node Node; typedef struct Var Var;
struct Token { int kind, len, value, line; const char *loc; };
struct Var { char name[NAME_MAX]; int offset, size, elem_size, count, global, init; };
struct Node {
    int kind, value, size, elem_size;
    Node *lhs, *rhs, *cond, *then, *els, *init, *inc, *body, *next;
    Var *var; char name[NAME_MAX];
};
static char src[SRC_MAX + 1], out[ASM_MAX + 1], strings[STR_MAX][256];
static int string_sizes[STR_MAX];
static Token toks[TOK_MAX]; static Node nodes[NODE_MAX];
static Var locals[VAR_MAX], globals[VAR_MAX];
static int src_len, out_len, ntok, pos, nnodes, nlocals, nglobals, nstrings;
static int label_id, failed, error_line, break_label, continue_label;
static const char *error_text;
static void fail_at(int line, const char *text) {
    if (!failed) { failed = 1; error_line = line; error_text = text; }
}
static int alpha(int c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int digit(int c) { return c>='0'&&c<='9'; }
static int alnum(int c) { return alpha(c)||digit(c); }
static int bytes_equal(const char*a,const char*b,int n){for(int i=0;i<n;i++)if(a[i]!=b[i])return 0;return 1;}
static int contains(const char*s,int c){while(*s)if(*s++==c)return 1;return 0;}
static int same(Token *t,const char *s) {
    int n=(int)strlen(s); return t->len==n&&bytes_equal(t->loc,s,n);
}
static void token(int k,const char *p,int n,int v,int line) {
    if(ntok>=TOK_MAX){fail_at(line,"too many tokens");return;}
    toks[ntok++]=(Token){k,n,v,line,p};
}
static int escaped(const char **at,int line) {
    const char *p=*at; if(*p!='\\'){*at=p+1;return(unsigned char)*p;}
    int c=*++p;
    if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='0')c=0;
    else if(c!='\\'&&c!='\''&&c!='"')fail_at(line,"unknown escape");
    *at=p+1;return c;
}
static void lex(void) {
    const char *p=src;int line=1,start=1;
    while(*p&&!failed){
        if(*p=='\n'){p++;line++;start=1;continue;}
        if(*p==' '||*p=='\t'||*p=='\r'){p++;continue;}
        if(start&&*p=='#'){while(*p&&*p!='\n')p++;continue;} start=0;
        if(p[0]=='/'&&p[1]=='/'){while(*p&&*p!='\n')p++;continue;}
        if(p[0]=='/'&&p[1]=='*'){p+=2;while(*p&&!(p[0]=='*'&&p[1]=='/'))if(*p++=='\n')line++;
            if(!*p){fail_at(line,"unterminated comment");break;}p+=2;continue;}
        if(alpha(*p)){const char*s=p++;while(alnum(*p))p++;token(TK_IDENT,s,(int)(p-s),0,line);continue;}
        if(digit(*p)){const char*s=p;int base=10,v=0;
            if(p[0]=='0'&&(p[1]=='x'||p[1]=='X')){base=16;p+=2;}else if(*p=='0'){base=8;p++;}
            for(;;){int d;if(digit(*p))d=*p-'0';else if(*p>='a'&&*p<='f')d=*p-'a'+10;
                else if(*p>='A'&&*p<='F')d=*p-'A'+10;else break;if(d>=base)break;v=v*base+d;p++;}
            while(*p=='u'||*p=='U'||*p=='l'||*p=='L')p++;token(TK_NUM,s,(int)(p-s),v,line);continue;}
        if(*p=='\''){const char*s=p++;int v=escaped(&p,line);if(*p++!='\''){fail_at(line,"bad character");break;}
            token(TK_NUM,s,(int)(p-s),v,line);continue;}
        if(*p=='"'){const char*s=p++;if(nstrings>=STR_MAX){fail_at(line,"too many strings");break;}int n=0;
            while(*p&&*p!='"'&&*p!='\n'&&n<255)strings[nstrings][n++]=(char)escaped(&p,line);
            if(*p++!='"'){fail_at(line,"bad string");break;}strings[nstrings][n]=0;string_sizes[nstrings]=n;
            token(TK_STRING,s,(int)(p-s),nstrings++,line);continue;}
        static const char*ops[]={"==","!=","<=",">=","&&","||","<<",">>",0};int hit=0;
        for(int i=0;ops[i];i++){int n=(int)strlen(ops[i]);if(bytes_equal(p,ops[i],n)){
            token(TK_PUNCT,p,n,0,line);p+=n;hit=1;break;}}if(hit)continue;
        if(contains("+-*/%(){}[]<>=!&|^~;,:",*p)){token(TK_PUNCT,p,1,0,line);p++;continue;}
        fail_at(line,"invalid character");
    } token(TK_EOF,p,0,0,line);
}
static Token*cur(void){return&toks[pos];}
static int take(const char*s){if(same(cur(),s)){pos++;return 1;}return 0;}
static void need(const char*s){if(!take(s))fail_at(cur()->line,"unexpected token");}
static void getname(char*d,Token*t){if(t->len>=NAME_MAX){fail_at(t->line,"name too long");return;}
    memcpy(d,t->loc,(size_t)t->len);d[t->len]=0;}
static int is_type(void){return same(cur(),"int")||same(cur(),"char")||same(cur(),"void")||
    same(cur(),"long")||same(cur(),"short")||same(cur(),"unsigned")||same(cur(),"signed")||
    same(cur(),"const")||same(cur(),"static");}
static int base_type(void){while(take("const")||take("static")||take("signed")||take("unsigned")){}
    if(take("char"))return 1;if(take("void"))return 0;if(take("long")||take("short")){take("int");return 4;}
    need("int");return 4;}
static Node*node(int k){if(nnodes>=NODE_MAX){fail_at(cur()->line,"tree too large");return 0;}
    Node*n=&nodes[nnodes++];memset(n,0,sizeof(*n));n->kind=k;return n;}
static Node*binary(int k,Node*a,Node*b){Node*n=node(k);n->lhs=a;n->rhs=b;return n;}
static Var*find_var(Token*t){
    for(int i=nlocals-1;i>=0;i--)
        if((int)strlen(locals[i].name)==t->len&&bytes_equal(locals[i].name,t->loc,t->len))return&locals[i];
    for(int i=nglobals-1;i>=0;i--)
        if((int)strlen(globals[i].name)==t->len&&bytes_equal(globals[i].name,t->loc,t->len))return&globals[i];
    return 0;
}

/* parser */
static Node *expression(void);
static Node *statement(void);

static int declarator(int base,char name[NAME_MAX],int *count,int *elem) {
    int pointers=0;while(take("*"))pointers++;
    if(cur()->kind!=TK_IDENT){fail_at(cur()->line,"expected identifier");return 4;}
    getname(name,cur());pos++;
    int size=pointers?4:base;*elem=base;*count=0;
    if(take("[")){if(cur()->kind!=TK_NUM)fail_at(cur()->line,"expected array size");
        else{*count=cur()->value;pos++;}need("]");size=(*count)*base;}
    return size;
}
static Var *new_local(const char*name,int size,int elem,int count,int offset) {
    if(nlocals>=VAR_MAX){fail_at(cur()->line,"too many locals");return 0;}
    Var*v=&locals[nlocals++];memset(v,0,sizeof(*v));strcpy(v->name,name);
    v->size=size;v->elem_size=elem;v->count=count;v->offset=offset;return v;
}
static Node *var_node(Var*v){Node*n=node(N_VAR);n->var=v;n->size=v->size;n->elem_size=v->elem_size;return n;}
static Node *primary(void) {
    if(take("(")){Node*n=expression();need(")");return n;}
    if(cur()->kind==TK_NUM){Node*n=node(N_NUM);n->value=cur()->value;n->size=4;pos++;return n;}
    if(cur()->kind==TK_STRING){Node*n=node(N_STRING);n->value=cur()->value;n->size=4;n->elem_size=1;pos++;return n;}
    if(cur()->kind!=TK_IDENT){fail_at(cur()->line,"expected expression");return node(N_NUM);}
    Token*t=cur();pos++;
    if(take("(")){Node*n=node(N_CALL);getname(n->name,t);Node head={0},*tail=&head;
        if(!take(")")){do{tail->next=expression();tail=tail->next;}while(take(","));need(")");}
        n->body=head.next;n->size=4;return n;}
    Var*v=find_var(t);if(!v){fail_at(t->line,"undefined variable");return node(N_NUM);}
    return var_node(v);
}
static Node *postfix(void) {
    Node*n=primary();
    while(take("[")){Node*i=expression();need("]");
        Node*scale=binary(N_MUL,i,node(N_NUM));scale->rhs->value=n->elem_size?n->elem_size:4;
        Node*addn=binary(N_ADD,n,scale);Node*d=node(N_DEREF);d->lhs=addn;
        d->size=n->elem_size?n->elem_size:4;n=d;}
    return n;
}
static Node *unary(void) {
    if(take("+"))return unary();
    if(take("-")){Node*n=node(N_NEG);n->lhs=unary();n->size=4;return n;}
    if(take("!")){Node*n=node(N_NOT);n->lhs=unary();n->size=4;return n;}
    if(take("~")){Node*n=node(N_BNOT);n->lhs=unary();n->size=4;return n;}
    if(take("&")){Node*n=node(N_ADDR);n->lhs=unary();n->size=4;n->elem_size=n->lhs->size;return n;}
    if(take("*")){Node*n=node(N_DEREF);n->lhs=unary();n->size=n->lhs->elem_size?n->lhs->elem_size:4;return n;}
    if(take("sizeof")){Node*n=node(N_NUM);n->size=4;
        if(take("(")&&is_type()){int b=base_type();while(take("*"))b=4;need(")");n->value=b;return n;}
        Node*x=unary();n->value=x->size?x->size:4;return n;}
    return postfix();
}
static Node *mul(void){Node*n=unary();for(;;){if(take("*"))n=binary(N_MUL,n,unary());
    else if(take("/"))n=binary(N_DIV,n,unary());else if(take("%"))n=binary(N_MOD,n,unary());else return n;}}
static Node *add(void){Node*n=mul();for(;;){if(take("+"))n=binary(N_ADD,n,mul());
    else if(take("-"))n=binary(N_SUB,n,mul());else return n;}}
static Node *shift(void){Node*n=add();for(;;){if(take("<<"))n=binary(N_SHL,n,add());
    else if(take(">>"))n=binary(N_SHR,n,add());else return n;}}
static Node *relation(void){Node*n=shift();for(;;){if(take("<"))n=binary(N_LT,n,shift());
    else if(take("<="))n=binary(N_LE,n,shift());else if(take(">"))n=binary(N_LT,shift(),n);
    else if(take(">="))n=binary(N_LE,shift(),n);else return n;}}
static Node *equality(void){Node*n=relation();for(;;){if(take("=="))n=binary(N_EQ,n,relation());
    else if(take("!="))n=binary(N_NE,n,relation());else return n;}}
static Node *bitand_expr(void){Node*n=equality();while(take("&"))n=binary(N_AND,n,equality());return n;}
static Node *bitxor_expr(void){Node*n=bitand_expr();while(take("^"))n=binary(N_XOR,n,bitand_expr());return n;}
static Node *bitor_expr(void){Node*n=bitxor_expr();while(take("|"))n=binary(N_OR,n,bitxor_expr());return n;}
static Node *land(void){Node*n=bitor_expr();while(take("&&"))n=binary(N_LAND,n,bitor_expr());return n;}
static Node *lor(void){Node*n=land();while(take("||"))n=binary(N_LOR,n,land());return n;}
static Node *assign(void){Node*n=lor();if(take("=")){Node*a=node(N_ASSIGN);a->lhs=n;a->rhs=assign();
    a->size=n->size;a->elem_size=n->elem_size;return a;}return n;}
static Node *expression(void){return assign();}

static Node *compound(void) {
    Node head={0},*tail=&head;
    while(!take("}")&&cur()->kind!=TK_EOF&&!failed){tail->next=statement();tail=tail->next;}
    Node*n=node(N_BLOCK);n->body=head.next;return n;
}
static Node *declaration(void) {
    int base=base_type();Node head={0},*tail=&head;
    do{char name[NAME_MAX];int count,elem,size=declarator(base,name,&count,&elem);
        int used=0;for(int i=0;i<nlocals;i++)if(locals[i].offset<0&&-locals[i].offset>used)used=-locals[i].offset;
        used=(used+size+3)&~3;Var*v=new_local(name,size,elem,count,-used);
        if(take("=")){Node*a=node(N_ASSIGN);a->lhs=var_node(v);a->rhs=expression();a->size=size;
            Node*s=node(N_EXPR);s->lhs=a;tail->next=s;tail=s;}
    }while(take(","));need(";");Node*n=node(N_BLOCK);n->body=head.next;return n;
}
static Node *statement(void) {
    if(is_type())return declaration();
    if(take("return")){Node*n=node(N_RETURN);if(!take(";")){n->lhs=expression();need(";");}return n;}
    if(take("if")){Node*n=node(N_IF);need("(");n->cond=expression();need(")");n->then=statement();
        if(take("else"))n->els=statement();return n;}
    if(take("while")){Node*n=node(N_WHILE);need("(");n->cond=expression();need(")");n->then=statement();return n;}
    if(take("for")){Node*n=node(N_FOR);need("(");if(!take(";")){n->init=expression();need(";");}
        if(!take(";")){n->cond=expression();need(";");}if(!take(")")){n->inc=expression();need(")");}
        n->then=statement();return n;}
    if(take("break")){need(";");return node(N_BREAK);}
    if(take("continue")){need(";");return node(N_CONTINUE);}
    if(take("{"))return compound();if(take(";"))return node(N_BLOCK);
    Node*n=node(N_EXPR);n->lhs=expression();need(";");return n;
}

/* code generator */
static void emit(const char*s){int n=(int)strlen(s);if(out_len+n>=ASM_MAX){fail_at(cur()->line,"output too large");return;}
    memcpy(out+out_len,s,(size_t)n);out_len+=n;out[out_len]=0;}
static void emit_num(int value){char b[16];int n=0;unsigned x;if(value<0){emit("-");x=(unsigned)(-(value+1))+1;}else x=(unsigned)value;
    do{b[n++]=(char)('0'+x%10);x/=10;}while(x);while(n--){char q[2]={b[n],0};emit(q);}}
static void emit_label(int id){emit("L");emit_num(id);}
static void gen_expr(Node*n);
static void gen_addr(Node*n){
    if(n->kind==N_VAR){if(n->var->global){emit("    mov eax, ");emit(n->var->name);emit("\n");}
        else{emit("    lea eax, [ebp");if(n->var->offset>=0)emit("+");emit_num(n->var->offset);emit("]\n");}return;}
    if(n->kind==N_DEREF){gen_expr(n->lhs);return;}fail_at(cur()->line,"not an lvalue");
}
static void load(Node*n){if(n->kind==N_VAR&&n->var->count)return;
    if(n->size==1)emit("    movzx eax, byte [eax]\n");else emit("    mov eax, dword [eax]\n");}
static void store(Node*n){if(n->size==1)emit("    mov byte [eax], bl\n");else emit("    mov dword [eax], ebx\n");}
static int argc_of(Node*n){int c=0;for(;n;n=n->next)c++;return c;}
static void gen_args(Node*n){if(!n)return;gen_args(n->next);gen_expr(n);emit("    push eax\n");}
static void gen_expr(Node*n){
    if(!n||failed)return;
    if(n->kind==N_NUM){emit("    mov eax, ");emit_num(n->value);emit("\n");return;}
    if(n->kind==N_STRING){emit("    mov eax, STR");emit_num(n->value);emit("\n");return;}
    if(n->kind==N_VAR){gen_addr(n);load(n);return;}
    if(n->kind==N_ADDR){gen_addr(n->lhs);return;}
    if(n->kind==N_DEREF){gen_expr(n->lhs);load(n);return;}
    if(n->kind==N_ASSIGN){gen_expr(n->rhs);emit("    push eax\n");gen_addr(n->lhs);emit("    pop ebx\n");
        store(n);emit("    mov eax, ebx\n");return;}
    if(n->kind==N_NEG||n->kind==N_NOT||n->kind==N_BNOT){gen_expr(n->lhs);
        if(n->kind==N_NEG)emit("    neg eax\n");else if(n->kind==N_BNOT)emit("    not eax\n");
        else emit("    cmp eax, 0\n    sete al\n    movzx eax, al\n");return;}
    if(n->kind==N_CALL){int c=argc_of(n->body);gen_args(n->body);emit("    call ");emit(n->name);emit("\n");
        if(c){emit("    add esp, ");emit_num(c*4);emit("\n");}return;}
    if(n->kind==N_LAND||n->kind==N_LOR){int a=label_id++,done=label_id++;gen_expr(n->lhs);emit("    cmp eax, 0\n");
        emit(n->kind==N_LAND?"    je ":"    jne ");emit_label(a);emit("\n");gen_expr(n->rhs);
        emit("    cmp eax, 0\n    setne al\n    movzx eax, al\n    jmp ");emit_label(done);emit("\n");
        emit_label(a);emit(n->kind==N_LAND?":\n    mov eax, 0\n":":\n    mov eax, 1\n");emit_label(done);emit(":\n");return;}
    gen_expr(n->lhs);emit("    push eax\n");gen_expr(n->rhs);emit("    mov ecx, eax\n    pop eax\n");
    if(n->kind==N_ADD)emit("    add eax, ecx\n");else if(n->kind==N_SUB)emit("    sub eax, ecx\n");
    else if(n->kind==N_MUL)emit("    imul eax, ecx\n");else if(n->kind==N_DIV||n->kind==N_MOD){
        emit("    cdq\n    idiv ecx\n");if(n->kind==N_MOD)emit("    mov eax, edx\n");}
    else if(n->kind==N_SHL)emit("    shl eax, cl\n");else if(n->kind==N_SHR)emit("    sar eax, cl\n");
    else if(n->kind==N_AND)emit("    and eax, ecx\n");else if(n->kind==N_XOR)emit("    xor eax, ecx\n");
    else if(n->kind==N_OR)emit("    or eax, ecx\n");else{emit("    cmp eax, ecx\n");
        if(n->kind==N_LT)emit("    setl al\n");else if(n->kind==N_LE)emit("    setle al\n");
        else if(n->kind==N_EQ)emit("    sete al\n");else if(n->kind==N_NE)emit("    setne al\n");
        emit("    movzx eax, al\n");}
}
static void gen_stmt(Node*n,int ret){
    if(!n||failed)return;if(n->kind==N_BLOCK){for(Node*s=n->body;s;s=s->next)gen_stmt(s,ret);return;}
    if(n->kind==N_EXPR){gen_expr(n->lhs);return;}if(n->kind==N_RETURN){if(n->lhs)gen_expr(n->lhs);else emit("    mov eax, 0\n");
        emit("    jmp ");emit_label(ret);emit("\n");return;}
    if(n->kind==N_IF){int e=label_id++,d=label_id++;gen_expr(n->cond);emit("    cmp eax, 0\n    je ");emit_label(e);emit("\n");
        gen_stmt(n->then,ret);emit("    jmp ");emit_label(d);emit("\n");emit_label(e);emit(":\n");gen_stmt(n->els,ret);
        emit_label(d);emit(":\n");return;}
    if(n->kind==N_WHILE||n->kind==N_FOR){int begin=label_id++,cont=label_id++,done=label_id++;
        int oldb=break_label,oldc=continue_label;break_label=done;continue_label=cont;
        if(n->kind==N_FOR&&n->init)gen_expr(n->init);emit_label(begin);emit(":\n");if(n->cond){gen_expr(n->cond);
            emit("    cmp eax, 0\n    je ");emit_label(done);emit("\n");}gen_stmt(n->then,ret);emit_label(cont);emit(":\n");
        if(n->kind==N_FOR&&n->inc)gen_expr(n->inc);emit("    jmp ");emit_label(begin);emit("\n");emit_label(done);emit(":\n");
        break_label=oldb;continue_label=oldc;return;}
    if(n->kind==N_BREAK||n->kind==N_CONTINUE){int l=n->kind==N_BREAK?break_label:continue_label;
        if(!l)fail_at(cur()->line,"break outside loop");else{emit("    jmp ");emit_label(l);emit("\n");}}
}
static void runtime(void){
    emit("global _start\nsection .text\n_start:\n    mov eax, dword [esp+8]\n    push eax\n");
    emit("    mov eax, dword [esp+8]\n    push eax\n    call main\n    add esp, 8\n");
    emit("    mov ebx, eax\n    mov eax, 1\n    int 0x80\n");
    emit("exit:\n    mov ebx, dword [esp+4]\n    mov eax, 1\n    int 0x80\n");
    emit("write:\n    mov ebx, dword [esp+4]\n    mov ecx, dword [esp+8]\n    mov edx, dword [esp+12]\n");
    emit("    mov eax, 5\n    int 0x80\n    ret\n");
    emit("read:\n    mov ebx, dword [esp+4]\n    mov ecx, dword [esp+8]\n    mov edx, dword [esp+12]\n");
    emit("    mov eax, 4\n    int 0x80\n    ret\n");
    emit("open:\n    mov ebx, dword [esp+4]\n    mov ecx, dword [esp+8]\n    mov eax, 2\n    int 0x80\n    ret\n");
    emit("close:\n    mov ebx, dword [esp+4]\n    mov eax, 3\n    int 0x80\n    ret\n");
    emit("putchar:\n    push ebp\n    mov ebp, esp\n    sub esp, 4\n    mov eax, dword [ebp+8]\n");
    emit("    mov byte [ebp-4], al\n    push 1\n    lea eax, [ebp-4]\n    push eax\n    push 1\n");
    emit("    call write\n    add esp, 12\n    leave\n    ret\n");
    emit("puts:\n    push ebp\n    mov ebp, esp\n    push esi\n    mov esi, dword [ebp+8]\n    mov edx, 0\n");
    emit("BCC_PUTS_LEN:\n    cmp byte [esi+edx], 0\n    je BCC_PUTS_WRITE\n    inc edx\n    jmp BCC_PUTS_LEN\n");
    emit("BCC_PUTS_WRITE:\n    push edx\n    push esi\n    push 1\n    call write\n    add esp, 12\n");
    emit("    push 1\n    mov eax, BCC_NEWLINE\n    push eax\n    push 1\n    call write\n    add esp, 12\n");
    emit("    pop esi\n    leave\n    ret\n");
}

/* translation unit and driver */
static int frame_size(void){int m=0;for(int i=0;i<nlocals;i++)if(locals[i].offset<0&&-locals[i].offset>m)m=-locals[i].offset;
    return(m+15)&~15;}
static void function(const char*name){
    nlocals=0;int off=8;
    if(!take(")")){
        if(same(cur(),"void")&&same(&toks[pos+1],")")){pos+=2;}
        else{do{int base=base_type();char pn[NAME_MAX];int count,elem,size=declarator(base,pn,&count,&elem);
            new_local(pn,size,elem,count,off);off+=4;}while(take(","));need(")");}
    }
    if(take(";")){nlocals=0;return;}
    need("{");Node*body=compound();int frame=frame_size(),ret=label_id++;
    emit(name);emit(":\n    push ebp\n    mov ebp, esp\n");if(frame){emit("    sub esp, ");emit_num(frame);emit("\n");}
    gen_stmt(body,ret);emit("    mov eax, 0\n");emit_label(ret);emit(":\n    leave\n    ret\n");nlocals=0;
}
static void program(void){
    runtime();
    while(cur()->kind!=TK_EOF&&!failed){
        if(!is_type()){fail_at(cur()->line,"expected declaration");break;}
        int base=base_type();char name[NAME_MAX];int count,elem,size=declarator(base,name,&count,&elem);
        if(take("(")){function(name);continue;}
        if(nglobals>=VAR_MAX){fail_at(cur()->line,"too many globals");break;}
        Var*v=&globals[nglobals++];memset(v,0,sizeof(*v));strcpy(v->name,name);v->global=1;
        v->size=size;v->elem_size=elem;v->count=count;
        if(take("=")){if(cur()->kind!=TK_NUM)fail_at(cur()->line,"global initializer must be constant");
            else{v->init=cur()->value;pos++;}}
        need(";");
    }
    emit("section .data\nBCC_NEWLINE: db 10\n");
    for(int i=0;i<nstrings;i++){emit("STR");emit_num(i);emit(": db ");
        for(int j=0;j<string_sizes[i];j++){if(j)emit(", ");emit_num((unsigned char)strings[i][j]);}
        if(string_sizes[i])emit(", ");emit("0\n");}
    for(int i=0;i<nglobals;i++){Var*v=&globals[i];emit(v->name);emit(": ");
        if(v->count){emit("times ");emit_num(v->count);emit(v->elem_size==1?" db 0\n":" dd 0\n");}
        else if(v->size==1){emit("db ");emit_num(v->init);emit("\n");}
        else{emit("dd ");emit_num(v->init);emit("\n");}}
}
static int read_source(const char*path){
    int fd=open(path,O_RDONLY);if(fd<0)return-1;src_len=0;
    while(src_len<SRC_MAX){int n=read(fd,src+src_len,(size_t)(SRC_MAX-src_len));if(n<=0)break;src_len+=n;}
    close(fd);if(src_len==SRC_MAX)return-1;src[src_len]=0;return 0;
}
static void default_output(const char*input,char*dest,int cap){
    int n=0;while(input[n]&&n<cap-1){dest[n]=input[n];n++;}dest[n]=0;
    if(n>2&&dest[n-2]=='.'&&dest[n-1]=='c')dest[n-2]=0;
}
int main(int argc,char**argv){
    if(argc<2){puts("usage: bcc <input.c> [output]");puts("example: bcc /fs/hello.c /fs/hello");return 1;}
    char output[128];if(argc>=3){if(strlen(argv[2])>=sizeof(output)){puts("bcc: output path too long");return 1;}
        strcpy(output,argv[2]);}else default_output(argv[1],output,sizeof(output));
    if(read_source(argv[1])<0){puts("bcc: cannot read source or source is too large");return 1;}
    out_len=ntok=pos=nnodes=nlocals=nglobals=nstrings=0;label_id=100;failed=0;error_line=1;error_text="compile failed";
    break_label=continue_label=0;lex();if(!failed)program();
    if(failed){printf("bcc: %s:%d: %s\n",argv[1],error_line,error_text);return 1;}
    if(basm_compile_source("bcc backend",out,out_len,output,0))return 1;
    printf("bcc: wrote %s\n",output);return 0;
}
