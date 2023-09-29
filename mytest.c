#include "types.h"
#include "user.h"
#include "stat.h"

int main(int argc, char const *argv[])
{
    //getname 관련
    int i=0;
    for(i=1; i<11; i++){
        printf(1, "%d:  ", i);
        if(getpname(i))
            printf(1, "Wrong pid\n");
    }

    //getnice관련
    for(i=1; i<11; i++){
        printf(1, "getnice 성공  %d\n", i);
        getnice(i);
    }
    prinf(2, "getnice 오류: 일치하는 pid 없음\n");
    getnice(1234);

    //setnice관련
    for(i=1; i<11; i++){
        printf(1, "setnice 성공  %d의 nice를 %d로 지정\n", i, i);
        setnice(i, i);
    }
    prinf(2, "setnice 오류 1: 일치하는 pid 없음\n");
    setnice(1234, 20);
    
    prinf(2, "setnice 오류 2: nice값이 범위를 넘어감\n");
    setnice(3, -1);

    //ps관련
    printf(1, "ps 성공 모든 프로세스 출력: \n");
    ps(0);

    printf(1, "ps pid가 1인 프로세스 출력\n");
    ps(1);

    printf(2, "ps 오류 pid가 1234인 프로세스 출력\n");
    ps(1234);


    exit();
}