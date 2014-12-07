
#include <assert.h>
#include <string.h>
#include <stdlib.h>

/*
#include <GLES2/gl2.h>
#include <EGL/egl.h>
*/

#ifdef RPI_NO_X
#include  "bcm_host.h"
#else
#include  <X11/Xlib.h>
#include  <X11/Xatom.h>
#include  <X11/Xutil.h>
#endif

#include "monitor.h"

#include <openedf.h>
#include <sys/time.h>
#include <pctimer.h>
#include <ctype.h>
#include <nsutil.h>
#include <nsnet.h>
#include <glib.h>

#include <fftw3.h>
#include <pthread.h>

#include "cube.h"
#include "esUtil.h"

typedef struct 
{
  float     avg; 
  int sampleBuf[2][SAMPLESIZE];
} THRDATA;

THRDATA threaddata;

pthread_t callThd[1];
pthread_mutex_t mutexsum;

/*
 * Vars related to polling neuroserver
 */
const char *helpText =
"sampleClient   v 0.34 by Rudi Cilibrasi\n"
"\n"
"Usage:  sampleClient [options]\n"
"\n"
"        -p port          Port number to use (default 8336)\n"
"        -n hostname      Host name of the NeuroCaster server to connect to\n"
"                         (this defaults to 'localhost')\n"
"        -e <intnum>      Integer ID specifying which EEG to log (default: 0)\n"
"The filename specifies the new EDF file to create.\n"
;

sock_t sock_fd;
char EDFPacket[MAXHEADERLEN];
GIOChannel *neuroserver;
//static int sampleBuf[2][SAMPLESIZE];
static int readSamples = 0;


static struct OutputBuffer ob;
struct InputBuffer ib;
char lineBuf[MAXLEN];
int linePos = 0;

struct Options opts;

/*
 * Thread: monitors eeg, calculates 'avg' for use in visualizations
 */
void *monitoreeg(void *arg) {
  int counter = 0;
  while (1)
  {
    counter++;
    //printf("counter %d", counter);
    idleHandler();

    if (counter % 50 == 0) {
      int i;
      fftw_complex *in;
      fftw_complex *in2;
      fftw_complex *out;
      fftw_plan plan_backward;
      fftw_plan plan_forward;

      in = fftw_malloc ( sizeof ( fftw_complex ) * SAMPLESIZE );

      for (i = 0; i < SAMPLESIZE; i++) {
        int val = threaddata.sampleBuf[0][i];
        in[i][0] = val;
        in[i][1] = 0;
      }

      /*
        printf ( "\n" );
        printf ( "  Input Data:\n" );
        printf ( "\n" );

        for ( i = 0; i < n; i++ )
        {
          printf ( "  %3d  %12f  %12f\n", i, in[i][0], in[i][1] );
        }
      */

      out = fftw_malloc ( sizeof ( fftw_complex ) * SAMPLESIZE );

      plan_forward = fftw_plan_dft_1d ( SAMPLESIZE, in, out, FFTW_FORWARD, FFTW_ESTIMATE );

      fftw_execute ( plan_forward );

      //printf ( "\n" );
      //printf ( "  Output Data:\n" );
      //printf ( "\n" );

      int num_freqs = 0;
      float avg = 0;
      for ( i = 0; i < SAMPLESIZE; i++ )
      {
        int hz;
        int val;
        val = abs(out[i][0]) ^ 2;
        hz = ((i * SAMPLESIZE) / 100);
        if (hz > 0 && hz < 45) {
          avg += val;
          num_freqs++;
          //printf ( "%3d  %5d  %12f  %12f\n", i, hz, out[i][0], out[i][1] );
          //printf ( "%3d  %5d  %12d\n", i, hz, val );
        }
      }
      avg = avg / num_freqs;
      printf("== AVG %f", avg);
      avg = avg * 0.000390625 + 0.005;

      /*
      if (avg > 0.5) { avg = 0.5; }
      if (avg < 0.05) { avg = 0.05; }
      */

      pthread_mutex_lock (&mutexsum);
      threaddata.avg = avg;
      pthread_mutex_unlock (&mutexsum);
      /*
       Oscope range: .1 - .5
       0.00048828125
      */
    }
  }
}


int main(int argc, char **argv)
{
  threaddata.avg = 0.1;

  int i;
  for (i = 0; i < SAMPLESIZE; i++) {
    threaddata.sampleBuf[0][i] = 0;
    threaddata.sampleBuf[1][i] = 0;
  }

  // Init eeg client
  char cmdbuf[80];
  int EDFLen = MAXHEADERLEN;
  struct EDFDecodedConfig cfg;
  double t0;
  int retval;
  strcpy(opts.hostname, DEFAULTHOST);
  opts.port = DEFAULTPORT;
  opts.eegNum = 0;
  for (i = 1; i < argc; ++i) {
    char *opt = argv[i];
    if (opt[0] == '-') {
      switch (opt[1]) {
        case 'h':
          printf("%s", helpText);
          exit(0);
          break;
        case 'e':
          opts.eegNum = atoi(argv[i+1]);
          i += 1;
          break;
        case 'n':
          strcpy(opts.hostname, argv[i+1]);
          i += 1;
          break;
        case 'p':
          opts.port = atoi(argv[i+1]);
          i += 1;
          break;
        }
      }
      else {
          fprintf(stderr, "Error: option %s not allowed", argv[i]);
          exit(1);
      }
  }

  rinitNetworking();

  sock_fd = rsocket();
  if (sock_fd < 0) {
      perror("socket");
      rexit(1);
  }
  rprintf("Got socket.\n");

  retval = rconnectName(sock_fd, opts.hostname, opts.port);
  if (retval != 0) {
      rprintf("connect error\n");
      rexit(1);
  }

  rprintf("Socket connected.\n");

  writeString(sock_fd, "display\n", &ob);
  getOK(sock_fd, &ib);
  rprintf("Finished display, doing getheader %d.\n", opts.eegNum);

  sprintf(cmdbuf, "getheader %d\n", opts.eegNum);
  writeString(sock_fd, cmdbuf, &ob);
  getOK(sock_fd, &ib);
  if (isEOF(sock_fd, &ib))
      serverDied();

  EDFLen = readline(sock_fd, EDFPacket, sizeof(EDFPacket), &ib);
//  rprintf("Got EDF Header <%s>\n", EDFPacket);
  readEDFString(&cfg, EDFPacket, EDFLen);

  sprintf(cmdbuf, "watch %d\n", opts.eegNum);
  writeString(sock_fd, cmdbuf, &ob);
  getOK(sock_fd, &ib);

  rprintf("Watching for EEG data.\n");

  // We're connected to nsd. Now initiate graphics
  ESContext esContext;
  UserData  userData;

  esInitContext ( &esContext );
  esContext.userData = &userData;

  esCreateWindow ( &esContext, "Meditation Cube", SCREENWID, SCREENHEI, ES_WINDOW_RGB );

  rprintf("About to init\n");

  if ( !Init ( &esContext ) )
     return 0;

  esRegisterDrawFunc ( &esContext, Draw );

  struct timeval t1, t2;
  struct timezone tz;
  float deltatime;
  float totaltime = 0.0f;
  unsigned int frames = 0;

  gettimeofday ( &t1 , &tz );

  pthread_attr_t attr;
  pthread_mutex_init(&mutexsum, NULL);
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  pthread_create(&callThd[0], &attr, monitoreeg, (void *)0);

  // Child process 1: show visualization
  while (userInterrupt(&esContext) == GL_FALSE)
  {
    /*
    monitoreeg("");
    monitoreeg("");
    monitoreeg("");
    monitoreeg("");
    */

    gettimeofday(&t2, &tz);
    deltatime = (float)(t2.tv_sec - t1.tv_sec + (t2.tv_usec - t1.tv_usec) * 1e-6);
    t1 = t2;

    if (esContext.updateFunc != NULL)
      esContext.updateFunc(&esContext, deltatime);
    if (esContext.drawFunc != NULL)
      esContext.drawFunc(&esContext);

    eglSwapBuffers(esContext.eglDisplay, esContext.eglSurface);

    totaltime += deltatime;
    frames++;
    if (totaltime >  2.0f)
    {
      printf("==== FPS=%3.4f %4d frames rendered in %1.4f seconds\n", frames/totaltime, frames, totaltime);
      totaltime -= 2.0f;
      frames = 0;
    }
  }

  int status = pthread_kill( callThd[0], SIGKILL);                                     
  if ( status <  0)
    perror("pthread_kill failed");

  pthread_attr_destroy(&attr);
  pthread_mutex_destroy(&mutexsum);
  pthread_exit(NULL);
}

void kill_child(int child_pid)
{
    kill(child_pid,SIGKILL);
}

void serverDied(void)
{
  rprintf("Server died!\n");
  exit(1);
}

/*
 * eeg-client-related functions
 */
void handleSample(int channel, int val)
{
  static int updateCounter = 0;
  assert(channel == 0 || channel == 1);
  if (!(val >= 0 && val < 1024)) {
    printf("Got bad value: %d\n", val);
    return;
    //exit(0);
  }

  /*
   * Fill buffer from left to right until you reach the end.
   *
   * Once there, instead keep shuffling sample values left by one and filling in the last slot (SAMPLESIZE-1) 
   */

  if (readSamples == SAMPLESIZE-1) {
    memmove(&threaddata.sampleBuf[channel][0], &threaddata.sampleBuf[channel][1],  sizeof(int)*(SAMPLESIZE-1));
  }

  threaddata.sampleBuf[channel][readSamples] = val;
  if (readSamples < SAMPLESIZE-1 && channel == 1)
    readSamples += 1;

}

void idleHandler(void)
{
  int i;
  char *cur;
  int vals[MAXCHANNELS + 5];
  int curParam = 0;
  int devNum, packetCounter, channels, *samples;

  linePos = readline(sock_fd, lineBuf, sizeof(EDFPacket), &ib);
  //rprintf("Got line retval=<%d>, <%s>\n", linePos, lineBuf);

  if (isEOF(sock_fd, &ib))
    exit(0);

  if (linePos < MINLINELENGTH)
    return;

  if (lineBuf[0] != '!')
    return;

  for (cur = strtok(lineBuf, DELIMS); cur ; cur = strtok(NULL, DELIMS)) {
    if (isANumber(cur))
        vals[curParam++] = atoi(cur);
// <devicenum> <packetcounter> <channels> data0 data1 data2 ...
    if (curParam < 3)
        continue;
    devNum = vals[0];
    packetCounter = vals[1];
    channels = vals[2];
    samples = vals + 3;
    for (i = 0; i < channels; ++i) {
      //rprintf("Sample #%d: %d\n", i, samples[i]);
    }
  }
//  rprintf("Got sample with %d channels: %d\n", channels, packetCounter);

  for (i = 0; i < 2; ++i)
    handleSample(i, samples[i]);
}

int isANumber(const char *str) {
  int i;
  for (i = 0; str[i]; ++i)
    if (!isdigit(str[i]))
      return 0;
  return 1;
}

gboolean readHandler(GIOChannel *source, GIOCondition cond, gpointer data)
{
    idleHandler();
    return TRUE;
}


/*
 * Drawing-related functions
 */
GLuint LoadTexture ( char *fileName )
{
   int width,
       height;
   char *buffer = esLoadTGA ( fileName, &width, &height );
   GLuint texId;

   if ( buffer == NULL )
   {
      esLogMessage ( "Error loading (%s) image.\n", fileName );
      return 0;
   }

   glGenTextures ( 1, &texId );
   glBindTexture ( GL_TEXTURE_2D, texId );

   glTexImage2D ( GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, buffer );
   glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
   glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
   glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT );  // GL_CLAMP_TO_EDGE
   glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT );  // GL_CLAMP_TO_EDGE

   free ( buffer );

   return texId;
}

GLuint LoadShaderDisk ( GLenum type, const char *filename )
{

  const GLchar* source = file_read(filename);
  if (source == NULL) {
    fprintf(stderr, "Error opening %s: ", filename); perror("");
    return 0;
  }

  printf("Source:\n\n%s", source);

  const GLchar* sources[2] = {
    "#version 100\n"
    "#define GLES2\n",
    source };

  GLuint shader;
  GLint compiled;
   
  // Create the shader object
  shader = glCreateShader ( type );

   if ( shader == 0 )
    return 0;

  glShaderSource(shader, 2, sources, NULL);
  free((void*)source);

   // Load the shader source
   //glShaderSource ( shader, 1, &shaderSrc, NULL );
   //glShaderSource ( shader, 1, &array, NULL );
   //glShaderSource ( shader, 1, (const GLchar**)&Src, 0 );
   
   // Compile the shader
   glCompileShader ( shader );

   // Check the compile status
   glGetShaderiv ( shader, GL_COMPILE_STATUS, &compiled );

   if ( !compiled ) 
   {
      GLint infoLen = 0;

      glGetShaderiv ( shader, GL_INFO_LOG_LENGTH, &infoLen );
      
      if ( infoLen > 1 )
      {
         char* infoLog = malloc (sizeof(char) * infoLen );

         glGetShaderInfoLog ( shader, infoLen, NULL, infoLog );
         esLogMessage ( "Error compiling shader:\n%s\n", infoLog );            
         
         free ( infoLog );
      }

      glDeleteShader ( shader );
      return 0;
   }

   return shader;

}


char* file_read(const char* filename)
{
  FILE* input = fopen(filename, "rb");
  if(input == NULL) return NULL;
 
  if(fseek(input, 0, SEEK_END) == -1) return NULL;
  long size = ftell(input);
  if(size == -1) return NULL;
  if(fseek(input, 0, SEEK_SET) == -1) return NULL;
 
  /*if using c-compiler: dont cast malloc's return value*/
  char *content = (char*) malloc( (size_t) size +1  ); 
  if(content == NULL) return NULL;
 
  fread(content, 1, (size_t)size, input);
  if(ferror(input)) {
    free(content);
    return NULL;
  }
 
  fclose(input);
  content[size] = '\0';
  return content;
}

///
// Initialize the shader and program object
//
int Init ( ESContext *esContext )
{
  rprintf("INIT.\n");

  esContext->userData = malloc(sizeof(UserData));

  UserData *userData = esContext->userData;

  GLuint vertexShader;
  GLuint fragmentShader;
  GLuint programObject;
  GLint linked;

  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) == NULL)
    perror("getcwd() error");

  char path_vertex[1024];
  strcpy(path_vertex, cwd);
  strcat(path_vertex, "/vertex.glsl");

  char path_fragment[1024];
  strcpy(path_fragment, cwd);
  strcat(path_fragment, "/calibrate.glsl");

  // Load the vertex/fragment shaders
  vertexShader = LoadShaderDisk ( GL_VERTEX_SHADER, path_vertex );
  fragmentShader = LoadShaderDisk ( GL_FRAGMENT_SHADER, path_fragment );

  // Create the program object
  programObject = glCreateProgram ( );
   
  if ( programObject == 0 )
     return 0;

  glAttachShader ( programObject, vertexShader );
  glAttachShader ( programObject, fragmentShader );

  // Bind vPosition to attribute 0   
  glBindAttribLocation ( programObject, 0, "vPosition" );

  // Link the program
  glLinkProgram ( programObject );

  // Check the link status
  glGetProgramiv ( programObject, GL_LINK_STATUS, &linked );

  srand(time(NULL));

  if ( !linked ) 
  {
     GLint infoLen = 0;

     glGetProgramiv ( programObject, GL_INFO_LOG_LENGTH, &infoLen );
      
     if ( infoLen > 1 )
     {
        char* infoLog = malloc (sizeof(char) * infoLen );
        glGetProgramInfoLog ( programObject, infoLen, NULL, infoLog );
        esLogMessage ( "Error linking program:\n%s\n", infoLog );            
        
        free ( infoLog );
     }

     glDeleteProgram ( programObject );
     return GL_FALSE;
  }

  // Store the program object
  userData->programObject = programObject;

  // Get the uniform locations
  userData->locGlobalTime = glGetUniformLocation( userData->programObject, "iGlobalTime" );
  userData->locIChannel0 = glGetUniformLocation( userData->programObject, "iChannel0" );
  userData->locYOffset = glGetUniformLocation( userData->programObject, "yOffset" );
  userData->locIResolution = glGetUniformLocation( userData->programObject, "iResolution");

  gettimeofday(&userData->timeStart, NULL);

  //userData->textureId = load_texture_TGA( "/home/ubuntu/openeeg/opengl_c/LinuxX11/Chapter_10/MultiTexture/tex16.tga", NULL, NULL, GL_REPEAT, GL_REPEAT );

  char path_tex16[1024];
  strcpy(path_tex16, cwd);
  strcat(path_tex16, "/tex16.tga");

  userData->textureId = LoadTexture( path_tex16 );


  if ( userData->textureId == 0 )
    return FALSE;

  glClearColor ( 0.0f, 0.0f, 0.0f, 1.0f );
  return GL_TRUE;
}

int mode = 0;

float CALIB_X_SCALE = 2.0;

///
// Draw a triangle using the shader pair created in Init()
//
void Draw ( ESContext *esContext )
{
  UserData *userData = esContext->userData;


  // Set the viewport
  glViewport ( 0, 0, esContext->width, esContext->height );
   
  // Clear the color buffer
  glClear ( GL_COLOR_BUFFER_BIT );

  // Use the program object
  glUseProgram ( userData->programObject );

  if (mode == 0) {
    GLfloat buffer[256*3];
    int i;
    int r;
    for (i = 0; i < SAMPLESIZE; i++) {
      if (i % 2 == 0) {
        printf("x: ");
        buffer[i] = i * 0.0078125 - 1;
      } else {
        printf("y: ");
        if (threaddata.sampleBuf[0][i] > 1024) {
          threaddata.sampleBuf[0][i] = 1024;
        }
        GLfloat val = ((float) threaddata.sampleBuf[0][i]) * 0.001953125 - 1;
        buffer[i] = val * CALIB_X_SCALE;
      }
      printf(" #%d: %f\t", i, buffer[i]);
      printf("\n");
    }

     // Load the vertex data
     glVertexAttribPointer ( 0, 2, GL_FLOAT, GL_FALSE, 0, buffer );
     glEnableVertexAttribArray ( 0 );

     glDrawArrays ( GL_LINE_STRIP, 0, 128 );    
  } else {
    // Completely cover the screen
    GLfloat vVertices1[] = { -1.0f, -1.0f, 0.0f, 
                             -1.0f, 1.0f, 0.0f,
                             1.0f, 1.0f, 0.0f,
                             1.0f, 1.0f, 0.0f,
                             1.0f, -1.0f, 0.0f,
                             -1.0f, -1.0f, 0.0f,
                            };

    // Load the vertex data
    glVertexAttribPointer ( 0, 3, GL_FLOAT, GL_FALSE, 0, vVertices1 );
    glEnableVertexAttribArray ( 0 );

    struct timeval timeNow, timeResult;
    gettimeofday(&timeNow, NULL);
    timersub(&timeNow, &userData->timeStart, &timeResult);
    float diffMs = (float)(timeResult.tv_sec + (timeResult.tv_usec / 1000000.0));

    //printf("Time elapsed: %ld.%06ld %f\n", (long int)timeResult.tv_sec, (long int)timeResult.tv_usec, diffMs);

     // Bind the texture
    glActiveTexture ( GL_TEXTURE0 );
    glBindTexture ( GL_TEXTURE_2D, userData->textureId );

    // Load the MVP matrix
    glUniform1f( userData->locGlobalTime, diffMs );

    // Set the sampler texture unit to 0
    glUniform1i ( userData->locIChannel0, 0 );

    //avg = (rand() % 10 + 1) / 10.0;
    printf("\navg %f", threaddata.avg);

    // Set the sampler texture unit to 0
    glUniform1f ( userData->locYOffset, threaddata.avg );

    glUniform2f( userData->locIResolution, SCREENWID, SCREENHEI );

    glDrawArrays ( GL_TRIANGLE_STRIP, 0, 18 );    
  }

}
