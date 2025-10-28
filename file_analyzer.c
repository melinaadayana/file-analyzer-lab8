#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <time.h>
#include <errno.h>

// ----------------------------------------------------------------------
// I. ESTRUCTURAS DE DATOS GLOBALES
// ----------------------------------------------------------------------

// DeclaraciÃ³n de Banderas Globales (Flags)
int g_opt_inode = 0;   // Activa impresiÃ³n de Inodo
int g_opt_perms = 0;   // Activa impresiÃ³n de Permisos (rwxr-xr-x)
int g_opt_size = 0;    // Activa impresiÃ³n de TamaÃ±o legible (KB, MB)
int g_opt_hash = 0;    // Activa el cÃ¡lculo de Hash (NO IMPLEMENTADO AQUÃ)
int g_opt_dupes = 0;   // Activa la DetecciÃ³n de duplicados

int g_total_files = 0;

// Estructura para almacenar las rutas de los archivos con el mismo Inodo/Hash
typedef struct PathNode {
    char path[1024];
    struct PathNode *next;
} PathNode;

// Estructura para la entrada de la Tabla Hash (clave = Inodo)
typedef struct DuplicateEntry {
    ino_t key_inode;        // El nÃºmero de inodo (la clave)
    int count;              // Contador de archivos con este inodo
    PathNode *file_paths;   // Lista enlazada de las rutas de estos archivos
} DuplicateEntry;

// Arreglo dinÃ¡mico para simular la Tabla Hash de duplicados
DuplicateEntry *g_dup_table = NULL;
int g_dup_table_size = 0;
int g_dup_table_capacity = 10; // Capacidad inicial

// ----------------------------------------------------------------------
// II. PROTOTIPOS DE FUNCIONES
// ----------------------------------------------------------------------

void human_readable_size(off_t size, char *output);
void permissions_to_string(mode_t mode, char *str);
void register_file_for_duplication(ino_t inode, const char *path);
void detect_duplicates();
void free_duplicate_table();
void analyze_directory(const char *path, int depth);

// ----------------------------------------------------------------------
// III. FUNCIONES DE UTILIDAD (FORMATO)
// ----------------------------------------------------------------------

/**
 * Convierte el tamaÃ±o de bytes a un formato legible (KB, MB, GB).
 */
void human_readable_size(off_t size, char *output) {
    const char *units[] = {"B", "K", "M", "G", "T"};
    int i = 0;
    double d_size = (double)size;

    while (d_size >= 1024.0 && i < 4) {
        d_size /= 1024.0;
        i++;
    }
    sprintf(output, "%.1f%s", d_size, units[i]);
}

/**
 * Traduce los bits de modo (st_mode) a la cadena rwxr-xr-x.
 */
void permissions_to_string(mode_t mode, char *str) {
    // Tipo de archivo: d (directorio), - (regular), l (symlink)
    str[0] = (S_ISDIR(mode)) ? 'd' : (S_ISLNK(mode)) ? 'l' : '-';

    // Permisos de propietario
    str[1] = (mode & S_IRUSR) ? 'r' : '-';
    str[2] = (mode & S_IWUSR) ? 'w' : '-';
    str[3] = (mode & S_IXUSR) ? 'x' : '-';

    // Permisos de grupo
    str[4] = (mode & S_IRGRP) ? 'r' : '-';
    str[5] = (mode & S_IWGRP) ? 'w' : '-';
    str[6] = (mode & S_IXGRP) ? 'x' : '-';

    // Permisos de otros
    str[7] = (mode & S_IROTH) ? 'r' : '-';
    str[8] = (mode & S_IWOTH) ? 'w' : '-';
    str[9] = (mode & S_IXOTH) ? 'x' : '-';
    str[10] = '\0';
}

// ----------------------------------------------------------------------
// IV. LÃ“GICA DE DUPLICADOS (INODO)
// ----------------------------------------------------------------------

/**
 * Registra la ruta de un archivo en la tabla de duplicados, agrupÃ¡ndolos por inodo.
 */
void register_file_for_duplication(ino_t inode, const char *path) {
    int i;
    
    // 1. Buscar si el Inodo ya estÃ¡ en la tabla
    for (i = 0; i < g_dup_table_size; i++) {
        if (g_dup_table[i].key_inode == inode) {
            
            // Inodo encontrado: aÃ±adir la ruta a la lista de paths
            PathNode *new_node = (PathNode *)malloc(sizeof(PathNode));
            if (!new_node) { perror("malloc PathNode"); return; }
            strcpy(new_node->path, path);
            new_node->next = g_dup_table[i].file_paths;
            g_dup_table[i].file_paths = new_node;
            g_dup_table[i].count++;
            return;
        }
    }

    // 2. Inodo NO encontrado: Crear una nueva entrada en la tabla
    if (g_dup_table_size >= g_dup_table_capacity) {
        g_dup_table_capacity *= 2;
        DuplicateEntry *temp = realloc(g_dup_table, sizeof(DuplicateEntry) * g_dup_table_capacity);
        if (!temp) { perror("realloc g_dup_table"); return; }
        g_dup_table = temp;
    }

    // Inicializar la nueva entrada
    g_dup_table[g_dup_table_size].key_inode = inode;
    g_dup_table[g_dup_table_size].count = 1;
    
    // Crear el primer nodo de la lista de paths
    PathNode *new_node = (PathNode *)malloc(sizeof(PathNode));
    if (!new_node) { perror("malloc PathNode"); return; }
    strcpy(new_node->path, path);
    new_node->next = NULL;
    g_dup_table[g_dup_table_size].file_paths = new_node;
    
    g_dup_table_size++;
}

/**
 * Recorre la tabla de duplicados e imprime los grupos con count > 1.
 */
void detect_duplicates() {
    int total_dupes = 0;
    int total_groups = 0;

    printf("\nArchivos duplicados encontrados (por inodo):\n");

    for (int i = 0; i < g_dup_table_size; i++) {
        // Un grupo es duplicado si tiene 2 o mÃ¡s archivos (hard links)
        if (g_dup_table[i].count > 1) {
            total_groups++;
            total_dupes += g_dup_table[i].count;
            
            printf("[inode: %lu]\n", (unsigned long)g_dup_table[i].key_inode);

            PathNode *current = g_dup_table[i].file_paths;
            while (current != NULL) {
                printf("â”œâ”€â”€ %s\n", current->path);
                current = current->next;
            }
        }
    }
    
    // Resumen final (requisito)
    printf("\nTotal archivos: %d\n", g_total_files); 
    printf("Archivos duplicados: %d (%d grupo%s)\n", 
           total_dupes, total_groups, total_groups != 1 ? "s" : "");
}

/**
 * Libera toda la memoria de las estructuras de duplicados.
 */
void free_duplicate_table() {
    for (int i = 0; i < g_dup_table_size; i++) {
        PathNode *current = g_dup_table[i].file_paths;
        PathNode *next;
        while (current != NULL) {
            next = current->next;
            free(current);
            current = next;
        }
    }
    if (g_dup_table) {
        free(g_dup_table);
    }
}

// ----------------------------------------------------------------------
// V. RECORRIDO RECURSIVO Y ANÃLISIS
// ----------------------------------------------------------------------

/**
 * FunciÃ³n recursiva que recorre el Ã¡rbol de directorios.
 */
void analyze_directory(const char *path, int depth) {
    DIR *dir;
    struct dirent *entry;
    struct stat st;
    char full_path[2048];
    char size_str[10];
    char perm_str[11];
    struct passwd *pw;
    struct group *gr;

    // Abrir directorio (Manejo de errores: directorio no vÃ¡lido/inaccesible)
    if (!(dir = opendir(path))) {
        fprintf(stderr, "Error: No se puede abrir/acceder al directorio %s (%s)\n", path, strerror(errno));
        return;
    }

    // Aplicar sangrÃ­a al nombre del directorio de inicio
    if (depth == 0) {
        printf("%s\n", path);
    }

    while ((entry = readdir(dir)) != NULL) {
        // Ignorar "." y ".." (Requisito)
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        // Construir la ruta completa
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        // Obtener metadatos con lstat (Requisito)
        if (lstat(full_path, &st) == -1) {
            fprintf(stderr, "Aviso: Archivo inaccesible o error en stat para %s (%s)\n", full_path, strerror(errno));
            continue;
        }

        // --- ImpresiÃ³n del listado ---
        // 1. Aplicar sangrÃ­a (indentaciÃ³n) segÃºn la profundidad
        for (int i = 0; i < depth; i++) printf("â”‚   ");
        printf("â”œâ”€â”€ ");

        // 2. Obtener metadatos formateados
        human_readable_size(st.st_size, size_str); // Requisito: Formato legible
        permissions_to_string(st.st_mode, perm_str); // Requisito: ConversiÃ³n de permisos
        pw = getpwuid(st.st_uid); // Requisito: Propietario
        gr = getgrgid(st.st_gid); // Requisito: Grupo
        
        // 3. Imprimir el listado (adaptar al formato del ejemplo)
        printf("(%c) %s", 
               S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : 'f', 
               entry->d_name);
        
        if (g_opt_size && !S_ISDIR(st.st_mode)) printf(" [%s]", size_str);
        if (g_opt_perms) printf(" [%s]", perm_str);
        if (g_opt_inode) printf(" [inode: %lu]", (unsigned long)st.st_ino);
        // if (g_opt_hash) printf(" [hash: %s]", hash_value); // (LÃ³gica de hash irÃ­a aquÃ­)
        printf("\n");
        

        // 4. Si es un directorio, llamar recursivamente
        if (S_ISDIR(st.st_mode)) {
            analyze_directory(full_path, depth + 1);
        } 
        // 5. Si es un archivo regular, registrar para duplicados y contar
        else if (S_ISREG(st.st_mode)) {
            g_total_files++;
            if (g_opt_dupes) {
                // Actualmente solo registra por inodo. Si se activa -h, se usarÃ­a hash
                register_file_for_duplication(st.st_ino, full_path);
            }
        }
    }

    closedir(dir);
}

// ----------------------------------------------------------------------
// VI. FUNCIÃ“N MAIN Y MANEJO DE OPCIONES
// ----------------------------------------------------------------------

int main(int argc, char *argv[]) {
    int opt;
    char *start_dir = NULL;

    // Inicializar la tabla de duplicados
    g_dup_table = (DuplicateEntry *)malloc(sizeof(DuplicateEntry) * g_dup_table_capacity);
    if (!g_dup_table) { perror("malloc g_dup_table"); return 1; }

    // Parseo de Opciones con getopt() (Requisito)
    // La cadena "ipshd" define las opciones vÃ¡lidas
    while ((opt = getopt(argc, argv, "ipshd")) != -1) {
        switch (opt) {
            case 'i':
                g_opt_inode = 1;
                g_opt_dupes = 1; // Activa detecciÃ³n al activar 'i' o 'h'
                break;
            case 'p':
                g_opt_perms = 1;
                break;
            case 's':
                g_opt_size = 1;
                break;
            case 'h':
                g_opt_hash = 1; 
                g_opt_dupes = 1;
                fprintf(stderr, "Aviso: La detecciÃ³n por Hash (-h) requiere la implementaciÃ³n de MD5/SHA1 y enlazar -lcrypto.\n");
                break;
            case 'd':
                g_opt_dupes = 1;
                break;
            case '?': // OpciÃ³n desconocida
                fprintf(stderr, "Uso: %s [-i|-p|-s|-h|-d] <directorio>\n", argv[0]);
                return 1;
        }
    }

    // Obtener el Argumento de Directorio (asume que es el Ãºltimo)
    if (optind < argc) {
        start_dir = argv[optind];
    }

    // VerificaciÃ³n y Manejo de Errores Iniciales (Requisito)
    if (start_dir == NULL) {
        fprintf(stderr, "Error: Debe especificar un directorio de inicio.\n");
        fprintf(stderr, "Uso: %s [opciones] <directorio>\n", argv[0]);
        free_duplicate_table();
        return 1;
    }

    // EjecuciÃ³n del AnÃ¡lisis Recursivo
    analyze_directory(start_dir, 0);

    // EjecuciÃ³n de DetecciÃ³n de Duplicados (solo si estÃ¡ activada) (Requisito)
    if (g_opt_dupes) {
        detect_duplicates();
    } else {
        // Resumen final si no se pidieron duplicados
        printf("\nTotal archivos: %d\n", g_total_files); 
    }

    // LiberaciÃ³n de memoria
    free_duplicate_table();

    return 0;
}
