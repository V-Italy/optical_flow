/**
  * Method as defined by Brox et al.

*/

#include "brox.hpp"
#define EPSILON 0.001

void debug(std::string text){
  std::cout << text << std::endl;
}


/**
  * Set up the parameters
*/
void setupParameters(std::unordered_map<std::string, parameter> &parameters){
  parameter alpha = {"alpha", 100, 1000, 1};
  parameter omega = {"omega", 195, 200, 100};
  parameter sigma = {"sigma", 15, 100, 10};
  parameter gamma = {"gamma", 500, 1000, 1000};
  parameter maxiter = {"maxiter", 200, 2000, 1};
  parameter maxlevel = {"maxlevel", 4, 10, 1};
  parameter wrapfactor = {"wrapfactor", 5, 10, 10};

  parameters.insert(std::make_pair<std::string, parameter>(alpha.name, alpha));
  parameters.insert(std::make_pair<std::string, parameter>(omega.name, omega));
  parameters.insert(std::make_pair<std::string, parameter>(sigma.name, sigma));
  parameters.insert(std::make_pair<std::string, parameter>(gamma.name, gamma));
  parameters.insert(std::make_pair<std::string, parameter>(maxiter.name, maxiter));
  parameters.insert(std::make_pair<std::string, parameter>(maxlevel.name, maxlevel));
  parameters.insert(std::make_pair<std::string, parameter>(wrapfactor.name, wrapfactor));
}


cv::Mat computeFlowField(const cv::Mat &image1, const cv::Mat &image2, std::unordered_map<std::string, parameter> &parameters){

  cv::Mat i1smoothed, i2smoothed, i1, i2, i2_wrap;
  int maxlevel = parameters.at("maxlevel").value;
  int maxiter = parameters.at("maxiter").value;
  double wrapfactor = (double)parameters.at("wrapfactor").value/parameters.at("wrapfactor").divfactor;
  double gamma = (double)parameters.at("gamma").value/parameters.at("gamma").divfactor;
  double sigma = (double)parameters.at("sigma").value/parameters.at("sigma").divfactor;
  double hx, hy;

  // make deepcopy, so images are untouched
  i1smoothed = image1.clone();
  i2smoothed = image2.clone();

  // convert to floating point images
  i1smoothed.convertTo(i1smoothed, CV_64F);
  i2smoothed.convertTo(i2smoothed, CV_64F);

  // blurring of the images (before resizing)
  cv::GaussianBlur(i1smoothed, i1smoothed, cv::Size(0,0), sigma, sigma, cv::BORDER_REFLECT);
  cv::GaussianBlur(i2smoothed, i2smoothed, cv::Size(0,0), sigma, sigma, cv::BORDER_REFLECT);

  // initialize parital and complete flowfield
  cv::Mat_<cv::Vec2d> partial(i1smoothed.size());
  cv::Mat_<cv::Vec2d> flowfield(i1smoothed.size());
  cv::Mat flowfield_wrap;

  // make a 2-channel matrix with each pixel with its coordinates as value (serves as basis for flowfield remapping)
  cv::Mat remap_basis(image1.size(), CV_32FC2);
  for (int i = 0; i < image1.rows; i++){
    for (int j = 0; j < image1.cols; j++){
      remap_basis.at<cv::Vec2f>(i,j)[0] = j;
      remap_basis.at<cv::Vec2f>(i,j)[1] = i;
    }
  }

  // loop for over levels
  for (int k = maxlevel; k >= 0; k--){

    // set steps in x and y-direction with 1.0/wrapfactor^level
    hx = 1.0/std::pow(wrapfactor, k);
    hy = hx;

    // scale to level, using area resampling
    cv::resize(i1smoothed, i1, cv::Size(0, 0), std::pow(wrapfactor, k), std::pow(wrapfactor, k), cv::INTER_AREA);
    cv::resize(i2smoothed, i2, cv::Size(0, 0), std::pow(wrapfactor, k), std::pow(wrapfactor, k), cv::INTER_AREA);

    // resample flowfield to current level (for now using area resampling)
    cv::resize(flowfield, flowfield, i1.size(), 0, 0, cv::INTER_AREA);
    cv::resize(partial, partial, i1.size(), 0, 0, cv::INTER_AREA);

    // wrap image 2 with current flowfield
    flowfield.convertTo(flowfield_wrap, CV_32FC2);
    flowfield_wrap = flowfield_wrap + remap_basis(cv::Rect(0, 0, flowfield_wrap.cols, flowfield_wrap.rows));
    cv::remap(i2, i2_wrap, flowfield_wrap, cv::Mat(), cv::INTER_LINEAR, cv::BORDER_TRANSPARENT, cv::Scalar(0));
    i2 = i2_wrap;

    // compute tensors
    cv::Mat_<cv::Vec6d> t = (1.0 - gamma) * ComputeBrightnessTensor(i1, i2, hy, hx) + gamma * ComputeGradientTensor(i1, i2, hx, hy);

    // add 1px border to flowfield, parital and tensor
    cv::copyMakeBorder(flowfield, flowfield, 1, 1, 1, 1, cv::BORDER_CONSTANT, 0);
    cv::copyMakeBorder(partial, partial, 1, 1, 1, 1, cv::BORDER_CONSTANT, 0);
    cv::copyMakeBorder(t, t, 1, 1, 1, 1, cv::BORDER_CONSTANT, 0);

    // set partial flowfield to zero
    partial = partial * 0;

    // main loop
    for (int i = 0; i < maxiter; i++){
      Brox_step_wo(t, flowfield, partial, parameters, hx, hy);
    }

    // add partial flowfield to complete flowfield
    flowfield = flowfield + partial;
  }
  return flowfield(cv::Rect(1, 1, image1.cols, image1.rows));
}




/**
  * Inner loop of the Brox et al method
  * (for now only use spatial smoothness term)
*/
void Brox_step(const cv::Mat_<cv::Vec6d> &t,
               const cv::Mat_<cv::Vec2d> &f,
               cv::Mat_<cv::Vec2d> &p,
               std::unordered_map<std::string, parameter> &parameters,
               double hx,
               double hy){

  // get parameters
  double alpha = (double)parameters.at("alpha").value/parameters.at("alpha").divfactor;
  double omega = (double)parameters.at("omega").value/parameters.at("omega").divfactor;

  // helper variables
  double xm, xp, ym, yp, sum, tmp;

  // compute the smoothness terms
  cv::Mat_<double> smooth = computeSmoothnessTerm(f, p, hx, hy);
  cv::Mat_<double> data = computeDataTerm(p, t);

  // update partial flow field
  for (int i = 1; i < p.rows - 1; i++){
    for (int j = 1; j < p.cols - 1; j++){

      // handle borders
      xm = (j > 1) * alpha/(hx*hx);
      xp = (j < p.cols - 1) * alpha/(hx*hx);
      ym = (i > 1) * alpha/(hy*hy);
      yp = (i < p.rows - 1) * alpha/(hy*hy);
      sum = xm + xp + ym + yp;



      // compute du
      // data terms
      tmp = data(i,j) * (t(i,j)[3] * p(i,j)[1] + t(i,j)[4]);

      // smoothness terms
      tmp = tmp
            - xm * (4.0 * smooth(i,j) - smooth(i,j+1) + smooth(i, j-1))/4.0 * (f(i,j-1)[0] + p(i,j-1)[0])
            - xp * (4.0 * smooth(i,j) + smooth(i,j+1) - smooth(i, j-1))/4.0 * (f(i,j+1)[0] + p(i,j+1)[0])
            - ym * (4.0 * smooth(i,j) - smooth(i+1,j) + smooth(i-1, j))/4.0 * (f(i-1,j)[0] + p(i-1,j)[0])
            - yp * (4.0 * smooth(i,j) + smooth(i+1,j) - smooth(i-1, j))/4.0 * (f(i+1,j)[0] + p(i+1,j)[0])
            + sum * smooth(i,j) * f(i,j)[0];

      // normalization
      tmp = tmp /(-data(i,j) * t(i,j)[0] - sum * smooth(i,j));
      p(i,j)[0] = (1.0-omega) * p(i,j)[0] + omega * tmp;


      // same for dv
      // data terms
      tmp = data(i,j) * (t(i,j)[3] * p(i,j)[0] + t(i,j)[5]);

      // smoothness terms
      tmp = tmp
            - xm * (4.0 * smooth(i,j) - smooth(i,j+1) + smooth(i, j-1))/4.0 * (f(i,j-1)[1] + p(i,j-1)[1])
            - xp * (4.0 * smooth(i,j) + smooth(i,j+1) - smooth(i, j-1))/4.0 * (f(i,j+1)[1] + p(i,j+1)[1])
            - ym * (4.0 * smooth(i,j) - smooth(i+1,j) + smooth(i-1, j))/4.0 * (f(i-1,j)[1] + p(i-1,j)[1])
            - yp * (4.0 * smooth(i,j) + smooth(i+1,j) - smooth(i-1, j))/4.0 * (f(i+1,j)[1] + p(i+1,j)[1])
            + sum * smooth(i,j) * f(i,j)[1];

      // normalization
      tmp = tmp /(-data(i,j) * t(i,j)[1] - sum * smooth(i,j));
      p(i,j)[1] = (1.0-omega) * p(i,j)[1] + omega * tmp;

    }
  }

}



void Brox_step_wo(const cv::Mat_<cv::Vec6d> &t,
               const cv::Mat_<cv::Vec2d> &f,
               cv::Mat_<cv::Vec2d> &p,
               std::unordered_map<std::string, parameter> &parameters,
               double hx,
               double hy){

  // get parameters
  double alpha = (double)parameters.at("alpha").value/parameters.at("alpha").divfactor;
  double omega = (double)parameters.at("omega").value/parameters.at("omega").divfactor;

  // helper variables
  double xm, xp, ym, yp, sum, tmp;

  // compute the smoothness terms
  //cv::Mat_<double> smooth = computeSmoothnessTerm(f, p, hx, hy);
  cv::Mat_<double> data = computeDataTerm(p, t);

  // update partial flow field
  for (int i = 1; i < p.rows - 1; i++){
    for (int j = 1; j < p.cols - 1; j++){

      // handle borders
      xm = (j > 1) * alpha/(hx*hx);
      xp = (j < p.cols - 2) * alpha/(hx*hx);
      ym = (i > 1) * alpha/(hy*hy);
      yp = (i < p.rows - 2) * alpha/(hy*hy);
      sum = xm + xp + ym + yp;



      // compute du
      // data terms
      tmp = data(i,j) * (t(i,j)[3] * p(i,j)[1] + t(i,j)[4]);

      // smoothness terms
      tmp = tmp
            - xm * (f(i,j-1)[0] + p(i,j-1)[0])
            - xp * (f(i,j+1)[0] + p(i,j+1)[0])
            - ym * (f(i-1,j)[0] + p(i-1,j)[0])
            - yp * (f(i+1,j)[0] + p(i+1,j)[0])
            + sum * f(i,j)[0];

      // normalization
      tmp = tmp /(- data(i,j) * t(i,j)[0] - sum);
      p(i,j)[0] = (1.0-omega) * p(i,j)[0] + omega * tmp;


      // same for dv
      // data terms
      tmp = data(i,j) * (t(i,j)[3] * p(i,j)[0] + t(i,j)[5]);

      // smoothness terms
      tmp = tmp
            - xm * (f(i,j-1)[1] + p(i,j-1)[1])
            - xp * (f(i,j+1)[1] + p(i,j+1)[1])
            - ym * (f(i-1,j)[1] + p(i-1,j)[1])
            - yp * (f(i+1,j)[1] + p(i+1,j)[1])
            + sum * f(i,j)[1];

      // normalization
      tmp = tmp /(- data(i,j) * t(i,j)[1] - sum);
      p(i,j)[1] = (1.0-omega) * p(i,j)[1] + omega * tmp;

    }
  }

}







cv::Mat computeSmoothnessTerm(const cv::Mat_<cv::Vec2d> &f, cv::Mat_<cv::Vec2d> &p, double hx, double hy){
  cv::Mat fc[2], pc[2];
  cv::Mat flow_u, flow_v, ux, uy, vx, vy, kernel, smoothness;

  // split flowfield in components
  cv::split(f, fc);
  flow_u = fc[0];
  flow_v = fc[1];

  // split partial flowfield in components
  cv::split(p, pc);
  flow_u = flow_u + pc[0];
  flow_v = flow_v + pc[1];

  // derivates in y-direction
  kernel = (cv::Mat_<double>(3,1) << -1, 0, 1);
  cv::filter2D(flow_u, uy, CV_64F, kernel * 1.0/(2*hy), cv::Point(-1,-1), 0, cv::BORDER_REPLICATE);
  cv::filter2D(flow_v, vy, CV_64F, kernel * 1.0/(2*hy), cv::Point(-1,-1), 0, cv::BORDER_REPLICATE);

  // derivates in x-dirction
  kernel = (cv::Mat_<double>(1,3) << -1, 0, 1);
  cv::filter2D(flow_u, ux, CV_64F, kernel * 1.0/(2*hx), cv::Point(-1,-1), 0, cv::BORDER_REPLICATE);
  cv::filter2D(flow_v, vx, CV_64F, kernel * 1.0/(2*hx), cv::Point(-1,-1), 0, cv::BORDER_REPLICATE);

  smoothness = ux.mul(ux) + uy.mul(uy) + vx.mul(vx) + vy.mul(vy);
  //std::cout << smoothness(cv::Rect(100,100,5,5)) << std::endl;
  //std::cout << ux(cv::Rect(100, 100, 5, 5)) << std::endl;
  for (int i = 0; i < smoothness.rows; i++){
    for (int j = 0; j < smoothness.cols; j++){
      smoothness.at<double>(i,j) = L1dot(smoothness.at<double>(i,j));
    }
  }

  return smoothness;
}


cv::Mat computeDataTerm(cv::Mat_<cv::Vec2d> &p, const cv::Mat_<cv::Vec6d> &t){
  cv::Mat_<double> data (p.size());
  double tmp;

  for (int i= 0; i < p.rows; i++){
    for (int j = 0; j < p.cols; j++){
      tmp =   t(i,j)[0] * p(i,j)[0] * p(i,j)[0]         // J11*du^2
            + t(i,j)[1] * p(i,j)[1] * p(i,j)[1]         // J22*dv^2
            + t(i,j)[2]                                 // J33
            + t(i,j)[3] * p(i,j)[0] * p(i,j)[1] * 2     // J21*du*dv
            + t(i,j)[4] * p(i,j)[0] * 2                 // J13*du
            + t(i,j)[5] * p(i,j)[1] * 2;                // J23*dv
      data(i,j) = L1dot(tmp);
    }
  }

  return data;
}


double L1(double value){
  return (value < 0 ) ? 0 : std::sqrt(value + EPSILON);
}


double L1dot(double value){
  value = value < 0 ? 0 : value;
  return 1.0/(2.0 * std::sqrt(value + EPSILON));
}
