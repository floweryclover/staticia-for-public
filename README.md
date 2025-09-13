# Staticia for public
* 공개 프로젝트로 진행하고 있지 않은 Staticia 프로젝트 중 공개하고자 하는 소스만을 담는 Repository

## 코드 파일 설명
|파일|설명|
|-|-|
|CAS_Bad_Cpu.h<br/>CAS_Bad_Cpu.cpp|워커 스레드 작업 분배를 CAS로 진행하여 높은 CPU 점유율을 얻었던 코드<br/>(cpp line 147)|
|FetchAdd_Good_Cpu.h<br/>FetchAdd_Good_Cpu.cpp|워커 스레드 작업 분배를 fetch add로 변경하여 CPU 사용량을 크게 개선했던 코드<br/>(cpp line 132)|
|ParallelExecutor.h|현재의 병렬 Executor<br/>템플릿과 concept를 이용한 Parallel For 함수들 구현<br/>스레드별 메모리 페이지를 이용한 경합 없는 결과 취합<br/>fetch add와 wait/notify_one만을 이용한 스레드 제어 및 동기화|
|ParallelExecutor.cpp|워커 스레드 body 구현|
|SparseSet.h|ECS 컴포넌트를 저장하는 Sparse set<br/>Dense Array와 Sparse Array를 이용한 빠른 순회와 임의 접근<br/>Swap-and-pop을 이용한 빠른 원소 삭제<br/>페이징과 placement new를 이용한 효율적 메모리 사용|
|ThreadRegistration.h|게임에서 사용할 스레드들에게 0~n-1의 연속적 번호를 부여하는 클래스<br/>ParallelExecutor나 Pathfinder 등에서 배열에 스레드별 공간을 할당하기 위해 활용 가능|
|G_Pathfinder.h|멀티스레드 A* 알고리즘을 위한 스레드 별 저장소 구현|
|G_Pathfinder.cpp|멀티스레드 A* 탐색 및 노드 생성 구현|