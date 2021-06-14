/*
 * NTAG21x.h
 *
 *  Created on: 11.06.2021
 *      Author: Noe Xu
 */

#ifndef ULTRAMAN_Z_H_
#define ULTRAMAN_Z_H_

#define ULTRAMANZ_CODE_PAGE 0x1F

enum {
    ULTRAMANZ_ACC_POS_TYPE = 0x1,
    ULTRAMANZ_ACC_NEG_TYPE = 0x2,
    ULTRAMANZ_CHAR_POS_TYPE = 0x4,
    ULTRAMANZ_CHAR_NEG_TYPE = 0x5
};

typedef struct {
    char type : 8;
    char index : 8;
    char type2 : 8;
    char sum : 8;
} Ultraman_Medal;

void Z_ACC_POS_ButtonFunc(void);
void Z_ACC_NEG_ButtonFunc(void);
void Z_CHAR_POS_ButtonFunc(void);
void Z_CHAR_NEG_ButtonFunc(void);
void UltramanAppInit(void);

#endif //ULTRAMAN_Z_H_
