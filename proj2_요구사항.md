
# proj2 요구사항
## 목적
### XV6 운영체제의 scheduling을 위한 nice value 처리 syscall 및 프로세스들의 정보 출력 syscall 구현

## 구현해야하는 것
### int getnice(int pid);
### int setnice(int pid, int value);
### void ps(int pid);

## 기본 조건
+ nice value는 값이 낮을수록 운영체제에서 먼저 스케줄링 되는 값
+ default nice vaule = 20
+ nice value 범위: 0~39

## 각 함수 조건
>### get nice: nice value를 가져오는 함수
>+ 일치하는 pid가 있으면 해당 프로세스의 nice value를 리턴
>+ 일치하는 pid가 없으면 -1을 리턴

>### set nice: nice value를 설정하는 함수
>+ set nice를 성공하면 0을 리턴
>+ 일치하는 pid가 없거나 nice value가 허용값을 초과하면 -1을 리턴

>### ps: 프로세스의 정보 출력(name, pid, state, nice valu(우선순위))
>+ 만약 pid가 0이면 모든 프로세스의 정보를 출력
>+ pid가 0이 아니면 해당 프로세스의 정보를 출력
>+ 해당하는 pid가 없으면 아무것도 출력x
>+ 리턴 값 없음

## system call을 만드는 방법
>1. usys.S에 시스템 콜 등록
>2. syscall.h 파일에 syscall number를 등록
>3. syscall.c에 추가하려는 systemcall 정보를 작성
>4. sysproc.c에 추가하려는 함수의 arguement들을 작성하고 return값을 통해 실제 함수를 호출, 함수의 이름은 'sys_함수이름' 으로 작성
>5. proc.c에 실제 함수의 코드를 작성
>6. defs.h와 user.h에 추가하려는 systemcall에 대한 정의를 작성

## 테스트 방법
>1. 임의의 테스트 코드인 test.c를 만들고 코드 내부에서 만든 syscall을 호출
>2. MakeFile의 마지막에 test\를 추가
>3. 다시 xv6를 빌드
>4. shell에서 test를 실행