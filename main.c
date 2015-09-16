#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <SDL2/SDL.h>
#include "main.h"
#include "dynamic_array.h"

#define SETPIXEL(x, y, value) ( *(pixels+(x)+(y)*WINDOW_WIDTH) = (value) )

//TODO(tony): find a better way for this
#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 400

Uint64 ID_start;
Uint64 ID_end;

//one character is 8x6; start in the top left corner, then scan vertically
const char characters[] = {
    63, 72, 72, 72, 63, 0, //A
    127, 73, 73, 73, 54, 0,//B
    62, 65, 65, 65, 65, 0,//C
    127, 65, 65, 65, 62, 0,//D
    127, 73, 73, 73, 65, 0,//E
    127, 72, 72, 72, 64, 0,//F
    49, 73, 73, 73, 70, 0,//S (for typing ASDF)
    0, 0, 0, 0, 0, 0, 0,//Space
};

unsigned int
get_string_length (char *buffer) //*not* counting the 0 at the end
{
    unsigned int length = 0;
    while (1)
    {
        if ( *(buffer+length) == 0 )
        {
            break;
        }
        else
        {
            length++;
        }
    }
    return length;
}


void
string_concat (char **buffer1, char **buffer2) //result will be in buffer1, both buffers must be 0-terminated
{
    unsigned int length1, length2, i;
    length1 = get_string_length(*buffer1);
    length2 = get_string_length(*buffer2);
    *buffer1 = realloc(*buffer1, length1+length2+1);
    for (i=0; i<=length2; i++) //the <= is so we copy over the 0 byte from buffer2, too
    {
        *(*buffer1+length1+i) = *(*buffer2+i);
    }
    return;
}

int
string_compare (char *buffer1, char *buffer2, int length)
{
    int i;
    for (i=0; i<length; i++)
    {
        if (buffer1[i] != buffer2[i])
        {
            return 0;
        }
    }
    return 1;
}

long //so we can have all the unsigned ints *and* return -1 on not finding an insert
getInsertByID (TextInsertSet *set, unsigned long selfID)
{
    unsigned int i;
    for (i=0; i<set->used_length; i++)
    {
        if ((set->array)[i].selfID == selfID)
        {
            return i;
        }
    }
    return -1;
}

Uint8
crc8_0x97 ( Uint8 *data, size_t length )
//Implemented following http://www.ross.net/crc/download/crc_v3.txt
//NOTE: switch to 0xA6 for bit lengths > 119 (polynomial data from http://ieeexplore.ieee.org/xpls/icp.jsp?arnumber=1311885#table3 )
{
    Uint8 crc_table[256];
    Uint8 poly = 0x97; //actually, we will read in 0x97 as the reversed reciprocal polynomial of what it actually is, but this will have no effect on error correction strength (https://en.wikipedia.org/wiki/Mathematics_of_cyclic_redundancy_checks#Reciprocal_polynomials)


    int i, j;
    for (i=0; i<256; i++)
    {
        Uint8 indexbyte = i;
        for (j=0; j<8; j++)
        {
            if ( (indexbyte >> 7) == 1 )
            {
                indexbyte <<= 1;
                indexbyte ^= poly;
            }
            else
            {
                indexbyte <<= 1;
            }
        }
        crc_table[i] = indexbyte;
    }

    Uint8 result = 0xFF;
    size_t k;
    for (k=0; k<length; k++)
    {
        result = crc_table[data[k] ^ result];
    }

    /* even more unreadable alternative
    while ( length-- > 0)
    {
        result = crc_table[(*data++)^result];
    }*/

    return result;
}

char
check_crc8_0x97 ( Uint8 *data, size_t length ) //checksum must be appended to data
{
    Uint8 crc_value = crc8_0x97(data, length);
    if ( crc_value == 0 )
    {
        return 0;
    }
    else
    {
        return -1;
    }
}

void
base85_enc_uint32 ( Uint32 input, DynamicArray_char *output )
{
    addToDynamicArray_char( output, 42 + ( input % 85 ) );
    input /= 85;

    addToDynamicArray_char( output, 42 + ( input % 85 ) );
    input /= 85;

    addToDynamicArray_char( output, 42 + ( input % 85 ) );
    input /= 85;

    addToDynamicArray_char( output, 42 + ( input % 85 ) );
    input /= 85;

    addToDynamicArray_char( output, 42 + ( input % 85 ) );

}

Uint32
base85_dec_uint32 ( char *input )
{
    Uint32 decoded = 0;

    decoded += input[4] - 42;
    decoded *= 85;
    
    decoded += input[3] - 42;
    decoded *= 85;

    decoded += input[2] - 42;
    decoded *= 85;

    decoded += input[1] - 42;
    decoded *= 85;

    decoded += input[0] - 42;

    return decoded;
}

void
serialize_insert ( TextInsert *insert, DynamicArray_char *output )
{
    unsigned int start_length = output->used_length;
    addToDynamicArray_char( output, 73 );// I for insert
    base85_enc_uint32( insert->selfID, output );
    base85_enc_uint32( insert->parentID, output );
    base85_enc_uint32( (insert->charPos << 16) + insert->length + (insert->lock << 31), output );
    int i;
    for (i=0; i < insert->length; i++)
    {
        addToDynamicArray_char( output, (insert->content)[i] );
    }
    Uint8 crc_value = crc8_0x97( output->array+start_length, output->used_length - start_length );
    addToDynamicArray_char(output, (crc_value>>4) + 65);
    addToDynamicArray_char(output, (crc_value&0x0F) + 65);
    //printf("serialize crc: %i\n", crc_value);
}

int
unserialize_insert ( TextInsertSet *set, char *string, size_t maxlength, size_t *return_insert_length)
{
    Uint32 mix = base85_dec_uint32( string+1+5+5 );
    int insert_content_length = mix&0xFFFF;
    size_t total_length = 1+5+5+5+insert_content_length+2;

    if (total_length > maxlength)
    {
        printf("Insert unserialization failed: invalid length.\n");
        return -1;
    }

    Uint8 checksum = ( (string[total_length-2] - 65) << 4) + (string[total_length-1] - 65);
    //printf("unserialize crc: %i\n", checksum);
    string[total_length-2] = checksum;
    if ( check_crc8_0x97( string, total_length-1 ) == -1 )
    {
        printf("Insert unserialization failed: invalid checksum.\n");
        return -1;
    }

    TextInsert insert;

    insert.selfID = base85_dec_uint32( string+1 );

    insert.parentID = base85_dec_uint32( string+1+5 );

    insert.charPos = (mix>>16)&0xFFFF;
    insert.length = insert_content_length;
    insert.lock = (mix>>31)&1;

    insert.content = malloc(insert.length);
    int i;
    for (i=0; i<insert.length; i++)
    {
        insert.content[i] = string[i+1+5+5+5];
    }

    long find_insert = getInsertByID(set, insert.selfID);
    if (find_insert != -1)
    {
        (set->array)[find_insert] = insert;
    }
    else
    {
        addToTextInsertSet(set, insert);
    }
    
    *return_insert_length = total_length;
    return 0;
}

int
send_insert ( TextInsert *insert, int write_fifo )
{
    DynamicArray_char message, messagelength;
    initDynamicArray_char(&message);
    initDynamicArray_char(&messagelength);

    addStringToDynamicArray_char(&message, "*****data");
    serialize_insert(insert, &message);
    base85_enc_uint32(message.used_length-5, &messagelength);

    //copy length into the message
    int i;
    for (i=0; i<5; i++)
    {
        message.array[i] = messagelength.array[i];
    }

    if (write(write_fifo, message.array, message.used_length) < 0)
    {
        printf("Sending insert failed.\n");
        return -1;
    }
    return 0;
}

void
serialize_document ( TextInsertSet *set, DynamicArray_char *output )
{
    int i;
    for (i=0; i < set->used_length; i++)
    {
        serialize_insert( (set->array) + i, output );
    }
}

int
unserialize_document ( TextInsertSet *set, char *string, size_t length )
{
    size_t insert_length;
    while ( length > 0 )
    {
        if ( unserialize_insert( set, string, length, &insert_length ) == -1 )
        {
            return -1;
        }
        string += insert_length;
        length -= insert_length;
    }
    return 0;
}

void
draw_text (TextBuffer *buffer, char *text, unsigned int x, unsigned int y, Uint32 *pixels, char show_cursor, FT_Face fontface)
{
    char character;
    char *index = text;
    char *char_after_cursor = text+buffer->cursor;
    unsigned int zero_x = x;
    int error;
    int height = (int) fontface->size->metrics.height / 64;

    while ((character=(*index++)))
    {
        if (character == 10) {
            x = zero_x;
            y += height;
        }
        else {
            error = FT_Load_Char( fontface, character, FT_LOAD_RENDER );

            FT_Bitmap bitmap = fontface->glyph->bitmap;

            if ( bitmap.pixel_mode != FT_PIXEL_MODE_GRAY )
            {
                printf("Not the right Freetype glyph bitmap pixel mode! Sorry, it ran on my computer...\n");
                return;
            }

            else
            {
                //copy glyph into pixel array

                unsigned int target_x, target_y;
                target_x = x + fontface->glyph->bitmap_left;
                target_y = y - fontface->glyph->bitmap_top;

                if ( bitmap.pitch < 0 )
                {
                    printf("Freetype glyph bitmap pitch is negative. Surely wasn't expecting that...\n");
                    return;
                }

                unsigned int row;
                unsigned int col;
                unsigned char *buffer = (unsigned char *) (bitmap.buffer);
                for ( row = 0; row < bitmap.rows; row++ )
                {
                    for ( col = 0; col < bitmap.width; col++ )
                    {
                        Uint32 color = *( buffer + row * (bitmap.pitch) + col );
                        SETPIXEL(target_x+col, target_y+row, (color<<24)+(color<<16)+(color<<8)+255);
                    }
                }
            }

            if ( show_cursor == 1 && index-1 == char_after_cursor ) //index-1 to compensate for the index++ in the for head
            {
                int i;
                for ( i=0; i<height; i++ )
                {
                    SETPIXEL(x, y-i+height/8, 0xFFFFFFFF);
                }
            }

            x += fontface->glyph->advance.x >> 6;
        }
    }

    if ( show_cursor == 1 && index-1 == char_after_cursor )
    {
        int i;
        for ( i=0; i<height; i++ )
        {
            SETPIXEL(x, y-i+height/8, 0xFFFFFFFF);
        }
    }
}

void
quicksort (unsigned long *array, unsigned long min, unsigned long max)
{
    if (min < max)
    {
        unsigned long pivot = min + (max-min)/2;
        unsigned long pivot_value = *(array+pivot);
        unsigned long lower_index = min;
        unsigned long temp_value = *(array+max);
        unsigned long i;
        *(array+max) = pivot_value;
        *(array+pivot) = temp_value;
        for (i = min; i < max; i++)
        {
            if ( *(array+i) < pivot_value )
            {
                if (i == lower_index)
                {
                    lower_index++;
                }
                else
                {
                    temp_value = *(array+lower_index);
                    *(array+lower_index) = *(array+i);
                    *(array+i) = temp_value;
                    lower_index++;
                }
            }
        }
        *(array+max) = *(array+lower_index);
        *(array+lower_index) = pivot_value;

        if (lower_index > 0)
        {
            quicksort(array, min, lower_index-1);
        }
        quicksort(array, lower_index+1, max);
    }
}


void
render_text (TextInsertSet *set, unsigned long parentID, unsigned short charPos, DynamicArray_char *output_buffer, DynamicArray_ulong *ID_table, DynamicArray_ulong *charPos_table)//output_buffer needs to be initialized; ID_table (needs to be initialized too) stores the ID of the insertion mark which contains each character, charPos_table stores the index of the character within its insertion mark. TODO: this is terribly inefficient with memory. fix sometime.
{
    unsigned int i;
    TextInsert current_insert;
    DynamicArray_ulong IDs;
    initDynamicArray_ulong(&IDs);

    for (i=0; i<set->used_length; i++)
    {
        TextInsert insert = *(set->array+i);
        if (insert.parentID == parentID && insert.charPos == charPos)
        {
            addToDynamicArray_ulong(&IDs, insert.selfID);
        }
    }

    //sort all the inserts
    if (IDs.used_length > 0)
    {
        quicksort(IDs.array, 0, IDs.used_length-1);
    }


    //render them in order

    for (i=0; i<IDs.used_length; i++)
    {
        //get insert
        current_insert = set->array[getInsertByID(set, IDs.array[i])];

        //draw it
        int pos;
        for (pos=0; pos<current_insert.length; pos++)
        {
            //render the inserts before this character position
            render_text(set, current_insert.selfID, pos, output_buffer, ID_table, charPos_table);

            //stick the appropriate letter on the back
            if (current_insert.content[pos] != 127)
            {
                addToDynamicArray_char(output_buffer, current_insert.content[pos]);
                addToDynamicArray_ulong(ID_table, current_insert.selfID);
                addToDynamicArray_ulong(charPos_table, pos);
            }
        }

        render_text(set, current_insert.selfID, current_insert.length, output_buffer, ID_table, charPos_table);
    }

    free(IDs.array);
}

void
update_buffer (TextInsertSet *set, DynamicArray_char *output_buffer, DynamicArray_ulong *ID_table, DynamicArray_ulong *charPos_table)
{
    //update buffer
    output_buffer->used_length = 0;
    ID_table->used_length = 0;
    charPos_table->used_length = 0;
    render_text(set, 0, 0, output_buffer, ID_table, charPos_table);
    addToDynamicArray_char(output_buffer, 0);
}


int
insert_letter (TextBuffer *buffer, TextInsertSet *set, DynamicArray_ulong *ID_table, DynamicArray_ulong *charPos_table, char letter, int write_fifo)
{

    if (buffer->activeInsertID)
    {
        TextInsert *insert = set->array + getInsertByID(set, buffer->activeInsertID);
        insert->content = realloc(insert->content, insert->length+1);
        insert->content[insert->length] = letter;
        insert->length++;

        send_insert(insert, write_fifo);
    }

    else
    {

        unsigned short pos = buffer->cursor;
        unsigned long insert_ID;
        unsigned short charPos;
        if ( pos == 0 )
        {
            insert_ID = 0;
            charPos = 0;
        }
        else
        {
            insert_ID = ID_table->array[pos-1];
            charPos = charPos_table->array[pos-1] + 1;
        }


        TextInsert new_insert;
        new_insert.selfID = ID_start++;
        if (ID_end < ID_start)
        {
            printf("Ran out of IDs!!!\n");
            exit(1);
        }
        new_insert.parentID = insert_ID;
        new_insert.charPos = charPos;
        new_insert.lock = 0;
        new_insert.length = 1;
        new_insert.content = malloc(1);
        new_insert.content[0] = letter;

        addToTextInsertSet(set, new_insert);
        buffer->activeInsertID = new_insert.selfID;
        
        send_insert(&new_insert, write_fifo);

    }

    buffer->cursor++;
    return 0;
}

int
delete_letter ( TextInsertSet *set, DynamicArray_ulong *ID_table, DynamicArray_ulong *charPos_table, unsigned short pos, int write_fifo )
{
    if (pos < ID_table->used_length)
    {
        unsigned long insert_ID = ID_table->array[pos];
        unsigned short inner_pos = 0;
        inner_pos = charPos_table->array[pos];

        TextInsert *insert = set->array + getInsertByID(set, insert_ID);
        insert->content[inner_pos] = 127;

        send_insert(insert, write_fifo);
    }
    return 0;
}

int main (void)
{

    int error;

    //fifo setup
    int read_channel, read_fifo, write_fifo;
    write_fifo = -1;

    error = mkfifo( "/tmp/deca_channel_1", 0777 );
    if (error == -1)
    {
        if ( errno == EEXIST )
        {
            //read channel is channel 2
            error = mkfifo( "/tmp/deca_channel_2", 0777 );
            read_channel = 2;

            if ( error == -1 )
            {
                printf("FIFO creation failed.\n");
                return 1;
            }

            write_fifo = open("/tmp/deca_channel_1", O_WRONLY);
            write(write_fifo, ".****Init", 9);
            read_fifo = open("/tmp/deca_channel_2", O_RDONLY | O_NONBLOCK);
        }

        else
        {
            printf("FIFO creation failed.\n");
            return 1;
        }
    }

    else
    {
        read_channel = 1;
        read_fifo = open("/tmp/deca_channel_1", O_RDONLY | O_NONBLOCK);
    }
    
    //Freetype setup
    
    FT_Library ft_library;
    error = FT_Init_FreeType( &ft_library);
    if (error)
    {
        printf("FreeType initialization failed.\n");
        return 1;
    }

    FT_Face fontface;
    error = FT_New_Face( ft_library, "ClearSans-Regular.ttf", 0, &fontface);
    if ( error )
    {
        printf("Font could not be loaded.\n");
        return 1;
    }

    error = FT_Set_Pixel_Sizes(fontface, 0, 24);

    if (read_channel == 1)
    {
        ID_start = 1;
        ID_end = 1024;
    }
    else
    {
        ID_start = 1025;
        ID_end = 2048;
    }

    //SDL setup

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL initialization failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("congame 3d test", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WINDOW_WIDTH, WINDOW_HEIGHT, 0);
    if (window == NULL) {
        SDL_Quit();
        printf("Window creation failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);
    if (renderer == NULL) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        printf("Renderer creation failed %s", SDL_GetError());
        return 1;
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);

    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, WINDOW_WIDTH, WINDOW_HEIGHT);

    Uint32 *pixels = malloc(WINDOW_WIDTH*WINDOW_HEIGHT*sizeof(Uint32));


    //Logic
    TextInsertSet set;
    initTextInsertSet(&set);
    //{
    //    TextInsert start_insert;
    //    start_insert.parentID = 0;
    //    start_insert.selfID = 1;
    //    start_insert.charPos = 0;
    //    start_insert.lock = 0;
    //    start_insert.length = 1;
    //    start_insert.content = malloc(1);
    //    start_insert.content[0] = 65; //<- WHEN DEBUGGING, WATCH THIS!!!
    //    addToTextInsertSet(&set, start_insert);
    //}


    DynamicArray_char output_buffer;
    DynamicArray_ulong ID_table;
    DynamicArray_ulong charPos_table;

    initDynamicArray_char(&output_buffer);
    initDynamicArray_ulong(&ID_table);
    initDynamicArray_ulong(&charPos_table);

    render_text(&set, 0, 0, &output_buffer, &ID_table, &charPos_table);
    addToDynamicArray_char(&output_buffer, 0);


    //main loop
    int quit=0;
    //int i;
    //int x, y;


    TextBuffer buffer;
    buffer.cursor = 0;
    buffer.activeInsertID = 0;

    //short unsigned cursor = 0;
    char unsigned blink_timer = 0;

    while (!quit)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            switch (e.type)
            {
                case SDL_QUIT:
                    quit = 1;
                    break;

                case SDL_TEXTINPUT:
                {
                    insert_letter(&buffer, &set, &ID_table, &charPos_table, e.text.text[0], write_fifo);
                    blink_timer = 0;

                    update_buffer(&set, &output_buffer, &ID_table, &charPos_table);
                } break;

                case SDL_KEYDOWN:
                {
                    switch (e.key.keysym.sym)
                    {
                        case SDLK_RETURN:
                        {
                            insert_letter(&buffer, &set, &ID_table, &charPos_table, 10, write_fifo);
                        } break;

                        case SDLK_BACKSPACE:
                        {
                            if (buffer.cursor > 0)
                            {
                                buffer.cursor--;
                                delete_letter (&set, &ID_table, &charPos_table, buffer.cursor, write_fifo);
                            }
                        } break;

                        case SDLK_RIGHT:
                        {
                            if (buffer.cursor < output_buffer.used_length-1)
                            {
                                buffer.cursor++;
                                buffer.activeInsertID = 0;
                            }
                        } break;

                        case SDLK_LEFT:
                        {
                            if (buffer.cursor > 0)
                            {
                                buffer.cursor--;
                                buffer.activeInsertID = 0;
                            }
                        } break;

                        case SDLK_ESCAPE:
                        {
                            quit = 1;
                        } break;
                    }

                    blink_timer = 0;

                    //update buffer
                    output_buffer.used_length = 0;
                    ID_table.used_length = 0;
                    charPos_table.used_length = 0;
                    render_text(&set, 0, 0, &output_buffer, &ID_table, &charPos_table);
                    addToDynamicArray_char(&output_buffer, 0);
                    //printf("Rendered text: %s\n # Inserts: %i\n", output_buffer.array, set.used_length);

                } break;

                default:
                    break;
            }
        }

        {
            Uint32 *pointer = pixels;
            while(pointer < pixels+WINDOW_WIDTH*WINDOW_HEIGHT)
            {
                *pointer = 0;
                pointer++;
            }
        }

        if (blink_timer < 128)
        {
            draw_text(&buffer, output_buffer.array, 10, 100, pixels, 1, fontface);
        }
        else
        {
            draw_text(&buffer, output_buffer.array, 10, 100, pixels, 0, fontface);
        }
        SDL_UpdateTexture(texture, NULL, pixels, WINDOW_WIDTH*sizeof(Uint32));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
        
        blink_timer+=9;
        SDL_Delay(30);

        //check FIFO
        {
            char *base85_length = malloc(5);
            if (read(read_fifo, base85_length, 5) == 5)
            {
                Uint32 length = base85_dec_uint32(base85_length);
                printf("received message of length %i\n", length);
                char *input = malloc(length);
                read(read_fifo, input, length);
                if (string_compare(input, "Init", 4) && write_fifo < 0)
                {
                    write_fifo = open("/tmp/deca_channel_2", O_WRONLY);
                }
                else if (string_compare(input, "data", 4))
                {
                    unserialize_document(&set, input+4, length-4);
                    update_buffer(&set, &output_buffer, &ID_table, &charPos_table);
                }
                free(input);
            }
        }
    }

    //insert serialization test
    DynamicArray_char serial_output;
    initDynamicArray_char( &serial_output );
    serialize_document( &set, &serial_output );

    SDL_DestroyWindow(window);
    SDL_Quit();
    free(pixels);

    FT_Done_FreeType(ft_library);

    if ( read_channel == 1)
    {
        remove( "/tmp/deca_channel_1" );
    }

    else
    {
        remove( "/tmp/deca_channel_2" );
    }

    return 0;
}
