#include <stdio.h>
#include <stdlib.h>

#define MAX_KNIGHT_NAME_LENGHT 20

typedef struct
{
    char name[MAX_KNIGHT_NAME_LENGHT + 1];
    int hp;
    int attack;
} knight;

/*
REFERENCES USED:

1. sop-roncevaux.c (starter file)
   - file reading logic (franci.txt / saraceni.txt)
   - knight struct definition
   - fscanf patterns

This stage DOES NOT use pipes yet, so no references to prog22a/b here.
*/

int stage1(knight** fr, knight** sa, int* nf, int* ns)
{
    FILE* franci = fopen("franci.txt", "r");
    if (!franci)
    {
        printf("Franks have not arrived on the battlefield\n");
        exit(EXIT_FAILURE);
    }

    FILE* saraceni = fopen("saraceni.txt", "r");
    if (!saraceni)
    {
        printf("Saracens have not arrived on the battlefield\n");
        fclose(franci);
        exit(EXIT_FAILURE);
    }

    /* number of knights */
    if (fscanf(franci, "%d", nf) != 1)
        exit(EXIT_FAILURE);

    *fr = malloc(sizeof(knight) * (*nf));
    if (!(*fr))
        exit(EXIT_FAILURE);

    for (int i = 0; i < *nf; i++)
    {
        if (fscanf(franci, "%20s %d %d",
                   (*fr)[i].name,
                   &(*fr)[i].hp,
                   &(*fr)[i].attack) != 3)
            exit(EXIT_FAILURE);
    }

    if (fscanf(saraceni, "%d", ns) != 1)
        exit(EXIT_FAILURE);

    *sa = malloc(sizeof(knight) * (*ns));
    if (!(*sa))
        exit(EXIT_FAILURE);

    for (int i = 0; i < *ns; i++)
    {
        if (fscanf(saraceni, "%20s %d %d",
                   (*sa)[i].name,
                   &(*sa)[i].hp,
                   &(*sa)[i].attack) != 3)
            exit(EXIT_FAILURE);
    }

    fclose(franci);
    fclose(saraceni);

    return 0;
}

/* test main for stage 1 */
int main(void)
{
    knight* fr = NULL;
    knight* sa = NULL;
    int nf = 0, ns = 0;

    stage1(&fr, &sa, &nf, &ns);

    printf("Franks:\n");
    for (int i = 0; i < nf; i++)
        printf("%s HP:%d ATK:%d\n", fr[i].name, fr[i].hp, fr[i].attack);

    printf("\nSaracens:\n");
    for (int i = 0; i < ns; i++)
        printf("%s HP:%d ATK:%d\n", sa[i].name, sa[i].hp, sa[i].attack);

    free(fr);
    free(sa);
    return 0;
}