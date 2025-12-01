Brand new pintos for Operating Systems and Lab (CS330), KAIST, by Youngjin Kwon.

The manual is available at https://casys-kaist.github.io/pintos-kaist/.

## Git Branch 전략
### main
- 팀원 모두가 합의한 최종 코드만 반영됩니다.
- ```main``` 브런치에서 직접 ```push``` 할 수 없습니다.

### hotfix
- ```main``` 브런치에서 에러가 발생한 경우, ```main``` 브런치에서 분기하여 바로 수정하고 반영합니다.

### feat
- 새로운 기능을 추가/수정/삭제합니다.
- ex) ```feat/pt```와 같이 세부 기능을 함께 명시합니다.

### refactor
- 기능은 동일하되, **성능 개선** 또는 **코드 가독성** 등을 위한 변경 사항을 반영합니다.
- ex) ```refactor/pt```와 같이 세부 기능을 함께 명시합니다.

### fix
- 버그를 수정합니다.
- ex) ```fix/pt```와 같이 세부 기능을 함께 명시합니다.

### docs
- 템플릿, README, 주석 등 문서 내용을 변경합니다.

### cicd
- CI/CD 설정을 변경합니다.

### chore
- 그 외 **잡다구리**한 일을 합니다.